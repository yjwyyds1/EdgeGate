#!/usr/bin/env python3
"""EdgeGate 端到端集成测试（仅标准库）。

覆盖：真实 HTTP/1.1 请求、keep-alive、连接池、chunked 请求/响应、
CL 与 chunked 上传、哈希校验、负载均衡、健康检查/故障转移、重试语义、
100-continue、管线化、HTTP/1.0、错误请求、管理接口、慢客户端背压。

运行：python3 tests/integration_test.py --binary-dir build
"""

import argparse
import hashlib
import json
import os
import random
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from collections import defaultdict

HOST = "127.0.0.1"


class Failure(RuntimeError):
    pass


# ---------------------------------------------------------------------------
# 基础设施
# ---------------------------------------------------------------------------

def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind((HOST, 0))
    port = s.getsockname()[1]
    s.close()
    return port


def wait_port(port, timeout=8.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.create_connection((HOST, port), timeout=0.2)
            s.close()
            return
        except OSError:
            time.sleep(0.03)
    raise RuntimeError(f"port {port} did not become ready")


class Proc:
    def __init__(self, name, cmd, log_path):
        self.name = name
        self.cmd = cmd
        self.log_path = log_path
        self.log = open(log_path, "wb")
        self.p = subprocess.Popen(cmd, stdout=self.log, stderr=subprocess.STDOUT)

    def stop(self, sig=signal.SIGTERM, timeout=5.0):
        if self.p.poll() is None:
            try:
                self.p.send_signal(sig)
            except ProcessLookupError:
                pass
        try:
            self.p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait(timeout=2)
        self.log.close()
        return self.p.returncode

    def tail(self, lines=80):
        try:
            with open(self.log_path, "r", errors="replace") as f:
                content = f.read().splitlines()
            return "\n".join(content[-lines:])
        except OSError:
            return "<no log>"


PROCS = []


def start(name, cmd, port=None, wait=True):
    log_path = os.path.join(LOGS, f"{name}.log")
    proc = Proc(name, cmd, log_path)
    PROCS.append(proc)
    if port is not None and wait:
        wait_port(port)
    return proc


# ---------------------------------------------------------------------------
# 最小 HTTP/1.1 客户端
# ---------------------------------------------------------------------------

class HttpError(RuntimeError):
    pass


class Resp:
    def __init__(self, version, status, reason, headers, body, closed):
        self.version = version
        self.status = status
        self.reason = reason
        self.headers = headers  # dict: lower name -> list[str]
        self.body = body
        self.closed = closed

    def get(self, name, default=None):
        vals = self.headers.get(name.lower())
        return vals[0] if vals else default

    def all(self, name):
        return self.headers.get(name.lower(), [])


class Conn:
    def __init__(self, port, timeout=10.0):
        self.sock = socket.create_connection((HOST, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buf = b""
        self.eof = False

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def _recv(self):
        if self.eof:
            return False
        data = self.sock.recv(65536)
        if not data:
            self.eof = True
            return False
        self.buf += data
        return True

    def _ensure(self, n):
        while len(self.buf) < n:
            if not self._recv():
                raise HttpError("connection closed while reading body")

    def _read_until(self, token):
        while token not in self.buf:
            if not self._recv():
                raise HttpError("connection closed while reading head")
        head, self.buf = self.buf.split(token, 1)
        return head

    def _read_exact(self, n):
        self._ensure(n)
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def _read_line(self):
        while b"\r\n" not in self.buf:
            if not self._recv():
                raise HttpError("connection closed while reading line")
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def send_raw(self, data):
        self.sock.sendall(data)

    def send_request(self, method, path, headers=None, body=None, version="HTTP/1.1",
                     chunked=False, host="edgegate"):
        hs = dict(headers or {})
        hs.setdefault("Host", host)
        hs.setdefault("Connection", "keep-alive")
        hs.setdefault("User-Agent", "eg-integration-test")
        raw = f"{method} {path} {version}\r\n".encode()
        for k, v in hs.items():
            raw += f"{k}: {v}\r\n".encode()
        if chunked:
            raw += b"Transfer-Encoding: chunked\r\n\r\n"
            if body:
                raw += f"{len(body):x}\r\n".encode() + body + b"\r\n"
            raw += b"0\r\n\r\n"
        else:
            if body is not None:
                raw += f"Content-Length: {len(body)}\r\n".encode()
            raw += b"\r\n"
            if body is not None:
                raw += body
        self.sock.sendall(raw)

    def read_response(self, skip_1xx=True, head_only=False):
        while True:
            head = self._read_until(b"\r\n\r\n")
            lines = head.split(b"\r\n")
            if not lines or not lines[0]:
                raise HttpError("empty status line")
            parts = lines[0].decode("latin1").split(" ", 2)
            if len(parts) < 2:
                raise HttpError(f"bad status line: {lines[0]!r}")
            version = parts[0]
            status = int(parts[1])
            reason = parts[2] if len(parts) > 2 else ""
            headers = defaultdict(list)
            for line in lines[1:]:
                if not line:
                    continue
                k, _, v = line.partition(b":")
                headers[k.decode("latin1").strip().lower()].append(
                    v.decode("latin1").strip())
            if 100 <= status < 200 and skip_1xx:
                continue

            body = b""
            no_body = (head_only or status in (204, 304) or
                       (100 <= status < 200))
            if not no_body:
                te = ",".join(headers.get("transfer-encoding", [])).lower()
                if "chunked" in te:
                    chunks = []
                    while True:
                        line = self._read_line()
                        size = int(line.split(b";")[0].strip(), 16)
                        if size == 0:
                            while True:
                                trailer = self._read_line()
                                if trailer == b"":
                                    break
                            break
                        chunks.append(self._read_exact(size))
                        if self._read_exact(2) != b"\r\n":
                            raise HttpError("bad chunk terminator")
                    body = b"".join(chunks)
                elif headers.get("content-length"):
                    body = self._read_exact(int(headers["content-length"][0]))
                else:
                    while self._recv():
                        pass
                    body, self.buf = self.buf, b""
            return Resp(version, status, reason, headers, body, self.eof)


def get_json(port, path, headers=None):
    c = Conn(port)
    c.send_request("GET", path, headers=headers)
    r = c.read_response()
    c.close()
    return r


def post_json(port, path, body=b"", headers=None):
    c = Conn(port)
    c.send_request("POST", path, headers=headers, body=body)
    r = c.read_response()
    c.close()
    return r


def metrics(port):
    r = get_json(port, "/metrics")
    out = {}
    for line in r.body.decode("utf-8", "replace").splitlines():
        if not line or line.startswith("#"):
            continue
        name, _, value = line.rpartition(" ")
        try:
            out[name] = float(value)
        except ValueError:
            pass
    return out


def metric_with_label(m, prefix):
    for k, v in m.items():
        if k.startswith(prefix):
            return v
    return 0.0


def curl(*args, timeout=20):
    env = os.environ.copy()
    env["no_proxy"] = "*"
    env["NO_PROXY"] = "*"
    proc = subprocess.run(["curl", "-sS", "-k", *map(str, args)],
                          capture_output=True, env=env, timeout=timeout)
    if proc.returncode != 0:
        raise Failure(f"curl failed ({proc.returncode}): {' '.join(map(str, args))}\n"
                      f"stdout={proc.stdout[:500]!r}\nstderr={proc.stderr[:500]!r}")
    return proc


def curl_supports_http2():
    if not shutil.which("curl"):
        return False
    try:
        out = subprocess.run(["curl", "--version"], capture_output=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return False
    return b"HTTP2" in out.upper()


def generate_cert(directory):
    os.makedirs(directory, exist_ok=True)
    cert = os.path.join(directory, "cert.pem")
    key = os.path.join(directory, "key.pem")
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
         "-keyout", key, "-out", cert, "-subj", "/CN=localhost",
         "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1"],
        check=True, capture_output=True, timeout=30)
    return cert, key


def rss_kb(pid):
    try:
        with open(f"/proc/{pid}/status", "r") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return 0


# ---------------------------------------------------------------------------
# 配置与进程
# ---------------------------------------------------------------------------

def write_config(name, listen, admin, backends, **opts):
    values = {
        "balancer": "round_robin",
        "max_attempts": 2,
        "preserve_host": "true",
        "health_interval_ms": 300,
        "health_timeout_ms": 200,
        "healthy_threshold": 1,
        "unhealthy_threshold": 1,
        "passive_failure_cooldown_ms": 300,
        "upstream_connect_timeout_ms": 1000,
        "upstream_response_timeout_ms": 8000,
        "client_header_timeout_ms": 3000,
        "client_idle_timeout_ms": 30000,
        "client_write_stall_ms": 5000,
        "max_request_header_bytes": 32768,
        "client_buffer_high_watermark": "256K",
        "client_buffer_low_watermark": "64K",
        "upstream_buffer_high_watermark": "256K",
        "upstream_buffer_low_watermark": "64K",
        "max_total_buffered_bytes": "128M",
        "max_connections_per_backend": 128,
        "max_idle_connections_per_backend": 32,
        "log_level": "info",
    }
    values.update(opts)
    lines = [f"listen = {HOST}:{listen}", f"admin_listen = {HOST}:{admin}"]
    for k, v in values.items():
        lines.append(f"{k} = {v}")
    for bname, bport in backends:
        lines.append(f"\n[backend {bname}]")
        lines.append(f"url = http://{HOST}:{bport}")
    path = os.path.join(CONFIGS, name + ".conf")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    return path


def start_backend(name, port, root, uploads):
    cmd = [os.path.join(BIN, "eg_demo"), "--listen", f"{HOST}:{port}",
           "--name", name, "--root", root, "--uploads", uploads, "--quiet"]
    return start(f"backend-{name}", cmd, port)


def start_proxy(name, cfg_path, port, admin_port):
    cmd = [os.path.join(BIN, "edgegate"), "--config", cfg_path, "--log-level", "info"]
    return start(name, cmd, port)


# ---------------------------------------------------------------------------
# Flaky 上游：读完请求后直接关闭，用来验证重试语义
# ---------------------------------------------------------------------------

class FlakyServer(threading.Thread):
    def __init__(self, port):
        super().__init__(daemon=True)
        self.port = port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((HOST, port))
        self.sock.listen(64)
        self.sock.settimeout(0.2)
        self.stop_flag = False
        self.connections = 0

    def run(self):
        while not self.stop_flag:
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            self.connections += 1
            try:
                conn.settimeout(1.0)
                data = b""
                while b"\r\n\r\n" not in data:
                    chunk = conn.recv(65536)
                    if not chunk:
                        break
                    data += chunk
                if b"\r\n\r\n" in data:
                    head, rest = data.split(b"\r\n\r\n", 1)
                    cl = 0
                    for line in head.split(b"\r\n")[1:]:
                        if line.lower().startswith(b"content-length:"):
                            cl = int(line.split(b":", 1)[1].strip())
                    while len(rest) < cl:
                        chunk = conn.recv(65536)
                        if not chunk:
                            break
                        rest += chunk
                # 读完整个请求后关闭，不给任何响应。
            except OSError:
                pass
            finally:
                try:
                    conn.close()
                except OSError:
                    pass

    def stop(self):
        self.stop_flag = True
        try:
            self.sock.close()
        except OSError:
            pass


# ---------------------------------------------------------------------------
# 测试主体
# ---------------------------------------------------------------------------

class Ctx:
    pass


def test_basic_and_lb(ctx):
    r = get_json(ctx.proxy, "/api/info")
    assert r.status == 200, r.status
    info = json.loads(r.body)
    assert info["instance"] in ctx.names
    assert r.get("x-edgegate-upstream") in ctx.names
    assert r.get("via") == "1.1 edgegate"
    assert r.get("x-edgegate-request-id")

    seen = set()
    for _ in range(12):
        r = get_json(ctx.proxy, "/api/info")
        assert r.status == 200
        seen.add(r.get("x-edgegate-upstream"))
    assert len(seen) == 3, f"round robin only hit {seen}"


def test_keepalive_and_pool(ctx):
    c = Conn(ctx.proxy)
    for i in range(8):
        c.send_request("GET", "/healthz")
        r = c.read_response()
        assert r.status == 200, r.status
        assert r.get("connection") == "keep-alive"
        assert json.loads(r.body)["status"] == "ok"
    c.close()

    m = metrics(ctx.admin)
    idle = sum(v for k, v in m.items() if k.startswith("edgegate_backend_idle_connections{"))
    assert idle >= 1, "upstream connections were not pooled"
    assert m.get("edgegate_requests_total", 0) >= 8


def test_chunked_response_and_close_delimited(ctx):
    c = Conn(ctx.proxy)
    c.send_request("GET", "/api/chunked?chunks=3&size=100")
    r = c.read_response()
    assert r.status == 200
    assert "chunked" in (r.get("transfer-encoding") or "").lower()
    assert len(r.body) == 300 and set(r.body) == {ord("z")}
    assert r.get("x-edgegate-upstream") in ctx.names

    c.send_request("GET", "/api/close")
    r = c.read_response()
    assert r.status == 200
    assert "chunked" in (r.get("transfer-encoding") or "").lower(), \
        "close-delimited upstream response should be re-chunked"
    assert b"close-delimited response" in r.body
    # 连接应该仍可复用：代理把上游的 close-delimited body 重新分帧了。
    c.send_request("GET", "/healthz")
    r = c.read_response()
    assert r.status == 200
    c.close()


def test_head_204_and_reuse(ctx):
    c = Conn(ctx.proxy)
    c.send_request("HEAD", "/api/bulk?mb=1")
    r = c.read_response(head_only=True)
    assert r.status == 200
    assert int(r.get("content-length")) == 1024 * 1024
    assert r.body == b""

    c.send_request("GET", "/api/no-content")
    r = c.read_response()
    assert r.status == 204
    assert r.body == b""

    c.send_request("GET", "/healthz")
    r = c.read_response()
    assert r.status == 200
    c.close()


def test_download_hash(ctx):
    path = os.path.join(ctx.root, "download.bin")
    with open(path, "wb") as f:
        f.write(os.urandom(3 * 1024 * 1024 + 123))
    with open(path, "rb") as f:
        expected = hashlib.sha256(f.read()).hexdigest()
    r = get_json(ctx.proxy, "/files/download.bin")
    assert r.status == 200, r.status
    assert hashlib.sha256(r.body).hexdigest() == expected
    assert len(r.body) == 3 * 1024 * 1024 + 123
    assert r.get("x-edgegate-upstream") in ctx.names


def test_upload_content_length_and_chunked(ctx):
    raw = os.urandom(1024 * 1024 + 7)
    expected = hashlib.sha256(raw).hexdigest()

    c = Conn(ctx.proxy)
    c.send_request("PUT", "/uploads/upload-cl.bin", body=raw)
    r = c.read_response()
    assert r.status == 200, r.status
    info = json.loads(r.body)
    assert info["bytes"] == len(raw)
    assert info["sha256"] == expected
    c.close()

    # 上传后可能被任何实例服务；共享 uploads 目录保证内容一致。
    r = get_json(ctx.proxy, "/uploads/upload-cl.bin")
    assert r.status == 200
    assert hashlib.sha256(r.body).hexdigest() == expected

    raw2 = os.urandom(512 * 1024 + 3)
    expected2 = hashlib.sha256(raw2).hexdigest()
    c = Conn(ctx.proxy)
    c.send_request("PUT", "/uploads/upload-chunked.bin", body=raw2, chunked=True)
    r = c.read_response()
    assert r.status == 200, r.status
    info = json.loads(r.body)
    assert info["sha256"] == expected2
    c.close()

    r = get_json(ctx.proxy, "/uploads/upload-chunked.bin")
    assert hashlib.sha256(r.body).hexdigest() == expected2

    # chunked 请求体 + 响应哈希
    echo = os.urandom(256 * 1024)
    c = Conn(ctx.proxy)
    c.send_request("POST", "/api/echo", body=echo, chunked=True)
    r = c.read_response()
    assert r.status == 200
    assert json.loads(r.body)["sha256"] == hashlib.sha256(echo).hexdigest()
    c.close()


def test_login_api(ctx):
    body = b'{"user":"demo","password":"demo"}'
    c = Conn(ctx.proxy)
    c.send_request("POST", "/api/login", body=body,
                   headers={"Content-Type": "application/json"})
    r = c.read_response()
    assert r.status == 200, (r.status, r.body)
    token = json.loads(r.body)["token"]
    c.send_request("GET", "/api/whoami", headers={"Authorization": f"Bearer {token}"})
    r = c.read_response()
    assert r.status == 200
    assert json.loads(r.body)["user"] == "demo"
    c.close()


def test_expect_100_continue(ctx):
    c = Conn(ctx.proxy)
    body = b"hello-continue"
    head = (b"POST /api/echo HTTP/1.1\r\nHost: test\r\nExpect: 100-continue\r\n"
            b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n")
    c.send_raw(head)
    r = c.read_response(skip_1xx=False)
    assert r.status == 100, r.status
    c.send_raw(body)
    r = c.read_response()
    assert r.status == 200
    assert json.loads(r.body)["sha256"] == hashlib.sha256(body).hexdigest()
    c.close()


def test_pipelining(ctx):
    c = Conn(ctx.proxy)
    req = (b"GET /healthz HTTP/1.1\r\nHost: test\r\nConnection: keep-alive\r\n\r\n")
    c.send_raw(req + req)
    r1 = c.read_response()
    r2 = c.read_response()
    assert r1.status == 200 and r2.status == 200
    assert json.loads(r1.body)["status"] == "ok"
    assert json.loads(r2.body)["status"] == "ok"
    c.close()


def test_http10(ctx):
    c = Conn(ctx.proxy)
    c.send_request("GET", "/healthz", version="HTTP/1.0",
                   headers={"Connection": "close"})
    r = c.read_response()
    assert r.version == "HTTP/1.0", r.version
    assert (r.get("connection") or "").lower() == "close"
    assert json.loads(r.body)["status"] == "ok"
    c.close()


def test_bad_requests(ctx):
    c = Conn(ctx.proxy)
    c.send_raw(b"POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\n"
               b"Transfer-Encoding: chunked\r\n\r\n")
    r = c.read_response()
    assert r.status == 400, r.status
    c.close()

    c = Conn(ctx.proxy)
    c.send_raw(b"GET / HTTP/1.1\r\nHost: a\r\n")
    c.send_raw(b"X-Big: " + b"x" * 40000 + b"\r\n\r\n")
    r = c.read_response()
    assert r.status == 431, r.status
    c.close()

    c = Conn(ctx.proxy)
    c.send_raw(b"GET / HTTP/1.1\r\n\r\n")
    r = c.read_response()
    assert r.status == 400, r.status
    c.close()


def test_admin_and_trace(ctx):
    r = get_json(ctx.admin, "/")
    assert r.status == 200
    assert b"EdgeGate" in r.body

    r = get_json(ctx.admin, "/api/status")
    assert r.status == 200
    status = json.loads(r.body)
    assert status["service"] == "edgegate"
    assert len(status["backends"]) == 3
    assert status["metrics"]["requests_total"] > 0

    r = get_json(ctx.admin, "/metrics")
    assert r.status == 200
    assert b"edgegate_request_duration_ms_bucket" in r.body

    # 用代理响应里的 request id 查询完整 trace。
    r = get_json(ctx.proxy, "/api/info")
    rid = r.get("x-edgegate-request-id")
    assert rid
    tr = get_json(ctx.admin, f"/api/requests/{rid}")
    data = json.loads(tr.body)
    assert data["found"] is True, data
    assert data["trace"]["id"] == rid
    assert data["trace"]["status"] == 200

    # 最近的请求列表
    recent = json.loads(get_json(ctx.admin, "/api/requests?limit=5").body)
    assert isinstance(recent, list) and len(recent) > 0


def test_drain_all_returns_503(ctx):
    for name in ctx.names:
        c = Conn(ctx.admin)
        c.send_request("POST", f"/api/backends/{name}/drain")
        r = c.read_response()
        c.close()
        assert r.status == 200, (name, r.status, r.body)
    r = get_json(ctx.proxy, "/api/info")
    assert r.status == 503, r.status
    for name in ctx.names:
        c = Conn(ctx.admin)
        c.send_request("POST", f"/api/backends/{name}/enable")
        r = c.read_response()
        c.close()
        assert r.status == 200
    r = get_json(ctx.proxy, "/api/info")
    assert r.status == 200, r.status


def test_failover_and_recovery(ctx):
    victim = ctx.backend_procs["app-b"]
    victim.stop()
    time.sleep(1.2)
    seen = set()
    for _ in range(12):
        r = get_json(ctx.proxy, "/api/info")
        assert r.status == 200, (r.status, r.body)
        seen.add(r.get("x-edgegate-upstream"))
    assert "app-b" not in seen, seen
    status = json.loads(get_json(ctx.admin, "/api/status").body)
    b = next(x for x in status["backends"] if x["name"] == "app-b")
    assert b["healthy"] is False, b

    if ctx.h2_available:
        for _ in range(8):
            p = curl("--http2", "-D", "-", "-o", "/dev/null",
                     tls_url(ctx, "/api/info"), timeout=10)
            assert b"HTTP/2 200" in p.stdout, p.stdout[:300]
            assert b"x-edgegate-upstream: app-b" not in p.stdout.lower()

    restarted = start_backend("app-b-restart", ctx.backend_ports["app-b"], ctx.root, ctx.uploads)
    ctx.backend_procs["app-b"] = restarted
    time.sleep(1.5)
    seen = set()
    for _ in range(20):
        r = get_json(ctx.proxy, "/api/info")
        assert r.status == 200
        seen.add(r.get("x-edgegate-upstream"))
    assert "app-b" in seen, seen

    if ctx.h2_available:
        h2_seen_b = False
        for _ in range(20):
            p = curl("--http2", "-D", "-", "-o", "/dev/null",
                     tls_url(ctx, "/api/info"), timeout=10)
            assert b"HTTP/2 200" in p.stdout
            if b"x-edgegate-upstream: app-b" in p.stdout.lower():
                h2_seen_b = True
        assert h2_seen_b


def test_backpressure(ctx):
    total_mb = 4
    total = total_mb * 1024 * 1024
    c = Conn(ctx.proxy, timeout=20)
    c.send_request("GET", f"/api/bulk?mb={total_mb}")
    # 先读响应头，再按慢速消费 body。
    while b"\r\n\r\n" not in c.buf:
        if not c._recv():
            raise AssertionError("proxy closed before response head")
    head, c.buf = c.buf.split(b"\r\n\r\n", 1)
    lines = head.split(b"\r\n")
    headers = {}
    for line in lines[1:]:
        k, _, v = line.partition(b":")
        headers[k.decode().lower()] = v.decode().strip()
    assert int(headers["content-length"]) == total

    expected = bytes(range(256)) * 3
    assert c.buf[:min(len(c.buf), 100)] == expected[:min(len(c.buf), 100)]
    print(f"    bp: after head buffered_in_client={len(c.buf)}")

    received = 0
    max_buffered = 0
    pauses = 0
    reads = 0
    rss = 0
    while received < total:
        if not c.buf:
            if not c._recv() and not c.buf:
                raise AssertionError(f"proxy closed early at {received}/{total} bytes")
        take = min(len(c.buf), 16384, total - received)
        c.buf = c.buf[take:]
        received += take
        reads += 1
        if reads % 50 == 0:
            m = metrics(ctx.admin)
            print(f"    bp: received={received}/{total} reads={reads} "
                  f"buffered={m.get('edgegate_buffered_bytes')} "
                  f"pauses={m.get('edgegate_upstream_read_paused_total')} "
                  f"resumes={m.get('edgegate_upstream_read_resumed_total')}")
        time.sleep(0.02)
        if reads % 10 == 0:
            m = metrics(ctx.admin)
            max_buffered = max(max_buffered, int(m.get("edgegate_buffered_bytes", 0)))
            pauses = max(pauses, int(m.get("edgegate_upstream_read_paused_total", 0)))
            rss = max(rss, rss_kb(ctx.proxy_proc.p.pid))
    assert received == total
    assert max_buffered < 2 * 1024 * 1024, f"proxy buffered {max_buffered} bytes"
    assert pauses > 0, "upstream read was never paused under a slow client"
    assert rss and rss < 300 * 1024, f"proxy RSS grew to {rss} KiB"
    # body 结束后续请求仍可复用连接
    c.send_request("GET", "/healthz")
    r = c.read_response()
    assert r.status == 200
    c.close()
    print(f"    backpressure: buffered<={max_buffered}B pauses={pauses} rss={rss}KiB")


def test_timeout_and_connect_failure(ctx):
    # 响应超时：后端 delay 1200ms，代理 400ms 放弃。
    proxy_port = free_port()
    admin_port = free_port()
    cfg = write_config("timeout", proxy_port, admin_port,
                       [("app-a", ctx.backend_ports["app-a"])],
                       health_interval_ms=0,
                       max_attempts=1,
                       upstream_response_timeout_ms=400)
    p = start_proxy("proxy-timeout", cfg, proxy_port, admin_port)
    try:
        r = get_json(proxy_port, "/api/delay?ms=1200")
        assert r.status == 504, (r.status, r.body)
        assert b"timeout" in r.body.lower()
    finally:
        p.stop()

    # 连接失败：端口无人监听。
    dead_port = free_port()
    proxy_port = free_port()
    admin_port = free_port()
    cfg = write_config("dead", proxy_port, admin_port,
                       [("dead", dead_port)],
                       health_interval_ms=0,
                       max_attempts=1,
                       upstream_connect_timeout_ms=300)
    p = start_proxy("proxy-dead", cfg, proxy_port, admin_port)
    try:
        r = get_json(proxy_port, "/healthz")
        assert r.status == 502, (r.status, r.body)
    finally:
        p.stop()


def test_retry_semantics(ctx):
    flaky_port = free_port()
    flaky = FlakyServer(flaky_port)
    flaky.start()

    def run(method, path, body, name):
        proxy_port = free_port()
        admin_port = free_port()
        cfg = write_config(name, proxy_port, admin_port,
                           [("flaky", flaky_port), ("good", ctx.backend_ports["app-a"])],
                           health_interval_ms=0,
                           unhealthy_threshold=100,
                           passive_failure_cooldown_ms=0,
                           upstream_response_timeout_ms=2000,
                           max_attempts=2)
        p = start_proxy("proxy-" + name, cfg, proxy_port, admin_port)
        try:
            c = Conn(proxy_port)
            headers = {"Content-Type": "application/octet-stream"}
            c.send_request(method, path, body=body, headers=headers)
            r = c.read_response()
            m = metrics(admin_port)
            c.close()
            return r, m
        finally:
            p.stop()

    try:
        body = os.urandom(4096)
        r, m = run("POST", "/api/echo", body, "retry-post")
        assert r.status == 502, (r.status, r.body)
        assert m.get("edgegate_retries_total", 0) == 0, "POST must not be retried"

        r, m = run("GET", "/healthz", None, "retry-get")
        assert r.status == 200, (r.status, r.body)
        assert m.get("edgegate_retries_total", 0) >= 1, "idempotent GET should be retried"
    finally:
        flaky.stop()


def tls_url(ctx, path):
    return f"https://{HOST}:{ctx.tls_port}{path}"


def test_tls_h1(ctx):
    if not ctx.tls:
        return
    p = curl("--http1.1", "-D", "-", "-o", "/dev/null", tls_url(ctx, "/api/info"))
    assert b"HTTP/1.1 200" in p.stdout, p.stdout[:300]
    assert b"x-edgegate-upstream" in p.stdout.lower()
    v = curl("--http1.1", "-o", "/dev/null", "-w", "%{http_version}",
             tls_url(ctx, "/api/info"))
    assert v.stdout.strip() == b"1.1", v.stdout


def test_h2_basic(ctx):
    if not ctx.h2_available:
        return
    p = curl("--http2", "-D", "-", "-o", "/dev/null", tls_url(ctx, "/api/info"))
    assert b"HTTP/2 200" in p.stdout, p.stdout[:300]
    assert b"x-edgegate-upstream" in p.stdout.lower()
    v = curl("--http2", "-o", "/dev/null", "-w", "%{http_version}",
             tls_url(ctx, "/api/info"))
    assert v.stdout.strip() == b"2", v.stdout

    body = os.urandom(256 * 1024)
    path = os.path.join(ctx.tmp, "h2-echo.bin")
    with open(path, "wb") as f:
        f.write(body)
    p = curl("--http2", "-X", "POST", "--data-binary", f"@{path}",
             tls_url(ctx, "/api/echo"))
    info = json.loads(p.stdout)
    assert info["bytes"] == len(body)
    assert info["sha256"] == hashlib.sha256(body).hexdigest()


def test_h2_parallel(ctx):
    if not ctx.h2_available:
        return
    out1 = os.path.join(ctx.tmp, "h2-par-1")
    out2 = os.path.join(ctx.tmp, "h2-par-2")
    m0 = metrics(ctx.admin).get("edgegate_client_connections_total", 0)
    started = time.monotonic()
    p = curl("--http2", "--parallel",
             "-o", out1, "-o", out2,
             "-w", "%{url_effective} %{http_version}\n",
             tls_url(ctx, "/api/delay?ms=400"), tls_url(ctx, "/api/info"),
             timeout=8)
    elapsed = time.monotonic() - started
    lines = [line for line in p.stdout.decode().splitlines() if line.strip()]
    assert len(lines) == 2, lines
    assert all(line.endswith(" 2") for line in lines), lines
    m1 = metrics(ctx.admin).get("edgegate_client_connections_total", 0)
    assert m1 - m0 == 1, (m0, m1)
    assert elapsed < 1.5, elapsed


def test_h2_transfer(ctx):
    if not ctx.h2_available:
        return
    p = curl("--http2", tls_url(ctx, "/api/chunked?chunks=3&size=1000"))
    assert len(p.stdout) == 3000

    p = curl("--http2", tls_url(ctx, "/api/close"))
    assert b"close-delimited response" in p.stdout

    p = curl("--http2", "-I", tls_url(ctx, "/api/bulk?mb=1"))
    assert b"content-length: 1048576" in p.stdout.lower(), p.stdout[:300]

    p = curl("--http2", "-o", "/dev/null", "-w", "%{http_code}",
             tls_url(ctx, "/api/no-content"))
    assert p.stdout.strip() == b"204"

    body = os.urandom(1024 * 1024)
    path = os.path.join(ctx.tmp, "h2-upload.bin")
    with open(path, "wb") as f:
        f.write(body)
    expected = hashlib.sha256(body).hexdigest()
    p = curl("--http2", "-X", "PUT", "--data-binary", f"@{path}",
             tls_url(ctx, "/uploads/h2-upload.bin"))
    assert json.loads(p.stdout)["sha256"] == expected
    p = curl("--http2", tls_url(ctx, "/uploads/h2-upload.bin"))
    assert hashlib.sha256(p.stdout).hexdigest() == expected

    p = curl("--http2", tls_url(ctx, "/api/bulk?mb=2"), timeout=20)
    assert len(p.stdout) == 2 * 1024 * 1024


def test_h2_body_limit(ctx):
    if not ctx.h2_available:
        return
    proxy_port = free_port()
    admin_port = free_port()
    tls_port = free_port()
    cfg = write_config(
        "h2-limit", proxy_port, admin_port,
        [("app-a", ctx.backend_ports["app-a"])],
        tls_listen=f"{HOST}:{tls_port}", tls_cert=ctx.cert, tls_key=ctx.key,
        http2="true", health_interval_ms=0, max_attempts=1,
        http2_max_request_body_bytes="64K",
        http2_max_total_request_body_bytes="1M")
    proc = start_proxy("proxy-h2-limit", cfg, proxy_port, admin_port)
    try:
        body = os.urandom(128 * 1024)
        path = os.path.join(ctx.tmp, "h2-too-large.bin")
        with open(path, "wb") as f:
            f.write(body)
        p = curl("--http2", "-D", "-", "-o", "/dev/null", "-X", "POST",
                 "--data-binary", f"@{path}",
                 f"https://{HOST}:{tls_port}/api/echo")
        assert b"HTTP/2 413" in p.stdout, p.stdout[:300]
    finally:
        proc.stop()


def test_h2_timeout(ctx):
    if not ctx.h2_available:
        return
    proxy_port = free_port()
    admin_port = free_port()
    tls_port = free_port()
    cfg = write_config(
        "h2-timeout", proxy_port, admin_port,
        [("app-a", ctx.backend_ports["app-a"])],
        tls_listen=f"{HOST}:{tls_port}", tls_cert=ctx.cert, tls_key=ctx.key,
        http2="true", health_interval_ms=0, max_attempts=1,
        upstream_response_timeout_ms=300)
    proc = start_proxy("proxy-h2-timeout", cfg, proxy_port, admin_port)
    try:
        p = curl("--http2", "-D", "-", "-o", "/dev/null",
                 f"https://{HOST}:{tls_port}/api/delay?ms=1200", timeout=8)
        assert b"HTTP/2 504" in p.stdout, p.stdout[:300]
    finally:
        proc.stop()


def test_h2_backpressure(ctx):
    if not ctx.h2_available:
        return
    out = os.path.join(ctx.tmp, "h2-slow.bin")
    env = os.environ.copy()
    env["no_proxy"] = "*"
    env["NO_PROXY"] = "*"
    proc = subprocess.Popen(
        ["curl", "-sS", "-k", "--http2", "--limit-rate", "256k", "-o", out,
         tls_url(ctx, "/api/bulk?mb=1")],
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    max_buffered = 0
    max_pauses = 0
    deadline = time.time() + 20
    try:
        while proc.poll() is None:
            if time.time() > deadline:
                proc.kill()
                raise Failure("h2 slow download did not finish")
            m = metrics(ctx.admin)
            max_buffered = max(max_buffered, int(m.get("edgegate_buffered_bytes", 0)))
            max_pauses = max(max_pauses, int(m.get("edgegate_upstream_read_paused_total", 0)))
            time.sleep(0.25)
    finally:
        if proc.poll() is None:
            proc.kill()
        proc.wait()
    assert proc.returncode == 0, proc.returncode
    assert os.path.getsize(out) == 1024 * 1024
    assert max_buffered < 2 * 1024 * 1024, max_buffered
    assert max_pauses > 0, "h2 response buffer never applied backpressure"


def test_h2_retry_semantics(ctx):
    if not ctx.h2_available:
        return

    def run(method, path, body, label):
        flaky_port = free_port()
        flaky = FlakyServer(flaky_port)
        flaky.start()
        proxy_port = free_port()
        admin_port = free_port()
        tls_port = free_port()
        cfg = write_config(
            label, proxy_port, admin_port,
            [("flaky", flaky_port), ("good", ctx.backend_ports["app-a"])],
            tls_listen=f"{HOST}:{tls_port}", tls_cert=ctx.cert, tls_key=ctx.key,
            http2="true", health_interval_ms=0, unhealthy_threshold=100,
            passive_failure_cooldown_ms=0, upstream_response_timeout_ms=2000,
            max_attempts=2)
        proc = start_proxy("proxy-" + label, cfg, proxy_port, admin_port)
        try:
            args = ["--http2", "-D", "-", "-o", "/dev/null"]
            if body is not None:
                path_file = os.path.join(ctx.tmp, label + ".bin")
                with open(path_file, "wb") as f:
                    f.write(body)
                args += ["-X", method, "--data-binary", f"@{path_file}"]
            p = curl(*args, f"https://{HOST}:{tls_port}{path}", timeout=8)
            m = metrics(admin_port)
            return p, m
        finally:
            proc.stop()
            flaky.stop()

    try:
        body = os.urandom(4096)
        p, m = run("POST", "/api/echo", body, "h2-retry-post")
        assert b"HTTP/2 502" in p.stdout, p.stdout[:300]
        assert m.get("edgegate_retries_total", 0) == 0, "HTTP/2 POST must not be retried"

        p, m = run("GET", "/healthz", None, "h2-retry-get")
        assert b"HTTP/2 200" in p.stdout, p.stdout[:300]
        assert m.get("edgegate_retries_total", 0) >= 1, "HTTP/2 GET should be retried"
    finally:
        pass


def test_graceful_shutdown(ctx):
    code = ctx.proxy_proc.stop()
    assert code in (0, -signal.SIGTERM), f"proxy exited with {code}"


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------

def main():
    global BIN, LOGS, CONFIGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary-dir", required=True)
    ap.add_argument("--filter", default="", help="只运行标签包含该字符串的测试")
    args = ap.parse_args()
    BIN = os.path.abspath(args.binary_dir)

    global TMP
    TMP = tempfile.mkdtemp(prefix="edgegate-it-")
    LOGS = os.path.join(TMP, "logs")
    CONFIGS = os.path.join(TMP, "configs")
    root = os.path.join(TMP, "root")
    uploads = os.path.join(TMP, "uploads")
    for d in (LOGS, CONFIGS, root, uploads):
        os.makedirs(d, exist_ok=True)

    ctx = Ctx()
    ctx.tmp = TMP
    ctx.root = root
    ctx.uploads = uploads
    ctx.names = ["app-a", "app-b", "app-c"]
    ctx.backend_ports = {name: free_port() for name in ctx.names}
    ctx.backend_procs = {}
    ctx.tls = False
    ctx.h2 = False
    ctx.h2_available = False
    ctx.cert = None
    ctx.key = None

    for name in ctx.names:
        ctx.backend_procs[name] = start_backend(name, ctx.backend_ports[name], root, uploads)

    ctx.proxy = free_port()
    ctx.admin = free_port()
    tls_opts = {}
    if shutil.which("curl") and shutil.which("openssl"):
        cert_dir = os.path.join(TMP, "certs")
        ctx.cert, ctx.key = generate_cert(cert_dir)
        ctx.tls_port = free_port()
        ctx.tls = True
        tls_opts = {
            "tls_listen": f"{HOST}:{ctx.tls_port}",
            "tls_cert": ctx.cert,
            "tls_key": ctx.key,
            "tls_min_version": "1.2",
            "http2": "true",
            "http2_max_request_body_bytes": "4M",
            "http2_max_total_request_body_bytes": "16M",
        }
        ctx.h2 = curl_supports_http2()

    cfg = write_config("main", ctx.proxy, ctx.admin,
                       [(n, ctx.backend_ports[n]) for n in ctx.names],
                       access_log=os.path.join(LOGS, "access.jsonl"),
                       **tls_opts)
    ctx.proxy_proc = start_proxy("proxy-main", cfg, ctx.proxy, ctx.admin)

    # 让健康检查先完成一轮，避免初始状态噪声。
    time.sleep(0.8)

    if ctx.tls and ctx.h2:
        try:
            probe = curl("--http2", "-o", "/dev/null", "-w", "%{http_version}",
                         tls_url(ctx, "/api/info"), timeout=8)
            ctx.h2_available = probe.stdout.strip() == b"2"
        except Failure:
            ctx.h2_available = False
        if not ctx.h2_available:
            print("    note: proxy did not negotiate HTTP/2; skipping h2 tests")

    tests = [
        ("basic + round robin", test_basic_and_lb),
        ("keep-alive + upstream pool", test_keepalive_and_pool),
        ("chunked response + close-delimited", test_chunked_response_and_close_delimited),
        ("HEAD / 204 / keep-alive", test_head_204_and_reuse),
        ("download hash", test_download_hash),
        ("upload CL + chunked + echo hash", test_upload_content_length_and_chunked),
        ("login API", test_login_api),
        ("Expect: 100-continue", test_expect_100_continue),
        ("pipelining", test_pipelining),
        ("HTTP/1.0", test_http10),
        ("bad requests", test_bad_requests),
        ("admin + request trace", test_admin_and_trace),
        ("drain all -> 503", test_drain_all_returns_503),
        ("failover + recovery", test_failover_and_recovery),
        ("slow client backpressure", test_backpressure),
        ("timeout + connect failure", test_timeout_and_connect_failure),
        ("retry semantics", test_retry_semantics),
        ("graceful shutdown", test_graceful_shutdown),
    ]

    if ctx.tls:
        tests.insert(-1, ("TLS HTTP/1.1", test_tls_h1))
    if ctx.h2_available:
        for label, fn in [
            ("HTTP/2 basic + POST", test_h2_basic),
            ("HTTP/2 multiplexed streams", test_h2_parallel),
            ("HTTP/2 chunked/HEAD/204/upload", test_h2_transfer),
            ("HTTP/2 request body limit", test_h2_body_limit),
            ("HTTP/2 upstream timeout", test_h2_timeout),
            ("HTTP/2 slow client backpressure", test_h2_backpressure),
            ("HTTP/2 retry semantics", test_h2_retry_semantics),
        ]:
            tests.insert(-1, (label, fn))

    passed = 0
    try:
        for label, fn in tests:
            if args.filter and args.filter not in label:
                continue
            print(f"[ RUN  ] {label}")
            fn(ctx)
            passed += 1
            print(f"[ PASS ] {label}")
    except Exception as e:
        print(f"\n[ FAIL ] after {passed}/{len(tests)} tests: {e}\n")
        for proc in PROCS:
            print(f"--- {proc.name} ({proc.log_path}) ---")
            print(proc.tail())
        for proc in PROCS:
            if proc.p.poll() is None:
                proc.stop(signal.SIGKILL, timeout=2)
        raise
    finally:
        for proc in PROCS:
            if proc.p.poll() is None:
                proc.stop(signal.SIGTERM, timeout=3)
    print(f"\nintegration_test: {passed}/{len(tests)} passed, workspace={TMP}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"integration_test failed: {exc}", file=sys.stderr)
        sys.exit(1)
