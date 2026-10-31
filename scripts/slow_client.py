#!/usr/bin/env python3
"""慢客户端：手动观察 EdgeGate 的背压。

用法：
  python3 scripts/slow_client.py --port 8080 --admin 9900 --mb 8 --chunk 16384 --sleep-ms 20
"""

import argparse
import socket
import time


def http(port, path):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.sendall(f"GET {path} HTTP/1.1\r\nHost: bench\r\nConnection: keep-alive\r\n\r\n".encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        data = s.recv(65536)
        if not data:
            raise SystemExit("connection closed before headers")
        buf += data
    head, body = buf.split(b"\r\n\r\n", 1)
    headers = {}
    for line in head.split(b"\r\n")[1:]:
        k, _, v = line.partition(b":")
        headers[k.decode().lower()] = v.decode().strip()
    return s, headers, body


def metrics(admin):
    s, _, body = http(admin, "/metrics")
    s.close()
    out = {}
    for line in body.decode().splitlines():
        if not line or line.startswith("#"):
            continue
        name, _, value = line.rpartition(" ")
        try:
            out[name] = float(value)
        except ValueError:
            pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--admin", type=int, default=9900)
    ap.add_argument("--path", default=None)
    ap.add_argument("--mb", type=int, default=8)
    ap.add_argument("--chunk", type=int, default=16384)
    ap.add_argument("--sleep-ms", type=float, default=20)
    args = ap.parse_args()
    if args.path is None:
        args.path = f"/api/bulk?mb={args.mb}"

    s, headers, body = http(args.port, args.path)
    total = int(headers.get("content-length", "0"))
    received = len(body)
    started = time.time()
    max_buffered = 0
    max_pauses = 0
    next_report = started + 1.0
    while received < total:
        data = s.recv(min(args.chunk, total - received))
        if not data:
            raise SystemExit(f"connection closed at {received}/{total}")
        received += len(data)
        time.sleep(args.sleep_ms / 1000.0)
        now = time.time()
        if now >= next_report:
            m = metrics(args.admin)
            max_buffered = max(max_buffered, int(m.get("edgegate_buffered_bytes", 0)))
            max_pauses = max(max_pauses, int(m.get("edgegate_upstream_read_paused_total", 0)))
            rate = received / (now - started) / 1024 / 1024
            print(f"received={received/1024/1024:.2f}/{total/1024/1024:.2f} MiB "
                  f"rate={rate:.2f} MiB/s buffered={int(m.get('edgegate_buffered_bytes', 0))} "
                  f"pauses={int(m.get('edgegate_upstream_read_paused_total', 0))}")
            next_report = now + 1.0
    s.sendall(b"GET /healthz HTTP/1.1\r\nHost: bench\r\nConnection: keep-alive\r\n\r\n")
    print(f"done: received={received} bytes, max_buffered={max_buffered}, max_pauses={max_pauses}")


if __name__ == "__main__":
    main()
