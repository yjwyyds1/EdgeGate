# EdgeGate 手工验收报告

- 日期：2026-11-01（v1.1 TLS/HTTP2 补充验收）
- 环境：WSL2 Ubuntu 26.04，4 vCPU，GCC 15.2.0，CMake 4.2.3，Ninja，OpenSSL，nghttp2，Debug/Release 均构建
- 范围：真实 HTTP/1.1 请求、文件上传/下载、chunked、keep-alive、管理端、故障转移、重试、背压；TLS 终止、ALPN、HTTP/2 多路复用、h2 请求体上限、h2 背压/超时/重试
- 自动化：`ctest --test-dir build-h2 --output-on-failure`，7/7 通过；集成测试 26/26

> 下列哈希来自一次真实运行；输入文件是随机生成的，换一次运行哈希会变，但“两端相等”这一结论可复现。

## 1. 构建与单元测试

```text
$ cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
$ cmake --build build -j
$ ctest --test-dir build --output-on-failure

1/7 buffer_test ...................... Passed
2/7 http_test ........................ Passed
3/7 config_test ...................... Passed
4/7 balancer_test .................... Passed
5/7 retry_test ....................... Passed
6/7 metrics_test ..................... Passed
7/7 integration_test ................. Passed
100% tests passed, 0 tests failed out of 7
```

## 2. 配置校验

```bash
./build/edgegate --config config/edgegate.conf --check-config
```

输出（节选）：

```text
config OK: listen=0.0.0.0:8080 admin=127.0.0.1:9900 balancer=least_conn
backends=3 max_attempts=2 preserve_host=true client_buffer=65536/262144
upstream_buffer=65536/262144 health_interval_ms=5000
backend[app-a]=127.0.0.1:9001 backend[app-b]=127.0.0.1:9002 backend[app-c]=127.0.0.1:9003
```

## 3. 真实请求

```bash
curl -sS -i http://127.0.0.1:8080/api/info
```

```http
HTTP/1.1 200 OK
date: Fri, 30 Oct 2026 07:50:06 GMT
server: eg-demo
x-backend: app-a
content-type: application/json; charset=utf-8
Content-Length: 122
Connection: keep-alive
Via: 1.1 edgegate
X-EdgeGate-Upstream: app-a
X-EdgeGate-Request-Id: eg-f982a0b718263d6f

{"instance":"app-a","pid":130080,"uptime_ms":814,"served":1,...}
```

再次请求命中 `app-b`，说明 round-robin/least-conn 在分发。

## 4. 文件下载与哈希

```bash
curl -sS http://127.0.0.1:8080/files/blob.bin -o /tmp/blob.out
sha256sum /tmp/eg-smoke/root/blob.bin /tmp/blob.out
```

一次运行：

```text
c28ca6efbbc64cb2d01a216a8ee46665b5543750fa6647647a269b6e6d8f28bb  /tmp/eg-smoke/root/blob.bin
c28ca6efbbc64cb2d01a216a8ee46665b5543750fa6647647a269b6e6d8f28bb  /tmp/blob.out
```

## 5. 上传、回读与哈希

```bash
head -c 1048576 /dev/urandom > /tmp/upload.bin
curl -sS -X PUT --data-binary @/tmp/upload.bin \
  http://127.0.0.1:8080/uploads/up.bin
curl -sS http://127.0.0.1:8080/uploads/up.bin -o /tmp/up.out
sha256sum /tmp/upload.bin /tmp/up.out
```

一次运行：

```text
{"instance":"app-a","name":"up.bin","bytes":1048576,
 "sha256":"aa3f76b47c75702dfa10ad41ff7a8e7da1e5993e3cd0d311c7b35afbb48900ab"}

aa3f76b47c75702dfa10ad41ff7a8e7da1e5993e3cd0d311c7b35afbb48900ab  /tmp/upload.bin
aa3f76b47c75702dfa10ad41ff7a8e7da1e5993e3cd0d311c7b35afbb48900ab  /tmp/up.out
```

上传由代理流式转发到后端；后端写临时文件、`fsync`、`rename`，再返回计算出的 SHA-256。回读可能命中另一实例，但共享 uploads 目录保证内容一致。

## 6. chunked 请求

```bash
printf 'hello-chunked' > /tmp/echo.txt
curl -sS -X POST -H 'Transfer-Encoding: chunked' \
  --data-binary @/tmp/echo.txt http://127.0.0.1:8080/api/echo
```

```json
{"instance":"app-a","bytes":13,
 "sha256":"8d9fb419edde4ae8d6940766689781b47294da38fd43e51282abf659d5a3528c"}
```

独立校验：

```text
8d9fb419edde4ae8d6940766689781b47294da38fd43e51282abf659d5a3528c  -
```

## 7. 管理端

```bash
curl -sS http://127.0.0.1:9900/api/status | head -c 400
curl -sS http://127.0.0.1:9900/metrics | grep edgegate_requests_total
```

```text
{"service":"edgegate","version":"1.0.0","uptime_seconds":0,
 "config":{...},"metrics":{"requests_total":7,"responses_total":7,...}}

# TYPE edgegate_requests_total counter
edgegate_requests_total 7
```

浏览器打开 `http://127.0.0.1:9900/` 可以看到后端状态、最近请求和 P95。

## 8. 故障转移与恢复

集成测试实际执行：

1. 杀掉 `app-b`；
2. 等待健康检查（300 ms 间隔、200 ms 超时、阈值 1）；
3. 连续 12 个请求只命中 `app-a/app-c`；
4. `/api/status` 中 `app-b.healthy=false`；
5. 重启 `app-b`；
6. 约 1.5 s 后 20 个请求中出现 `app-b`。

```text
[ PASS ] failover + recovery
```

## 9. 重试语义

flaky 上游读完请求后直接关闭：

```text
[ PASS ] retry semantics
```

- GET：重试到真实后端，200，`edgegate_retries_total >= 1`；
- POST：请求字节已写出，不重试，502，`edgegate_retries_total == 0`。

## 10. 慢客户端背压

```text
[ RUN  ] slow client backpressure
    bp: received=798720/4194304 reads=50 buffered=262144 pauses=11 resumes=10
    bp: received=1593344/4194304 reads=100 buffered=262144 pauses=14 resumes=13
    bp: received=2387968/4194304 reads=150 buffered=0 pauses=15 resumes=15
    backpressure: buffered<=262144B pauses=15 rss=5652KiB
[ PASS ] slow client backpressure
```

## 11. TLS 与 HTTP/2（v1.1）

### 11.1 TLS HTTP/1.1 与 ALPN

```text
$ bash scripts/tls_smoke.sh
=== TLS HTTP/1.1 ===
HTTP/1.1 200 OK
via: 1.1 edgegate
x-edgegate-upstream: app-a
=== TLS HTTP/2 GET ===
HTTP/2 200
=== TLS HTTP/2 version ===
http_version=2
=== TLS HTTP/2 bulk ===
2097152 /tmp/eg-h2-bulk.bin
=== ALPN / cipher ===
Protocol version: TLSv1.3
Ciphersuite: TLS_AES_256_GCM_SHA384
Peer certificate: CN=localhost
```

代理日志确认 ALPN 分流：

```text
[DEBUG] TLS handshake complete from 127.0.0.1 ALPN=h2
[INFO] HTTP/2 connection from 127.0.0.1 (ALPN h2)
[DEBUG] TLS handshake complete from 127.0.0.1 ALPN=http/1.1
```

### 11.2 HTTP/2 多路复用与特殊响应

```text
$ bash scripts/h2_manual.sh
=== h2 parallel (should multiplex) ===
https://127.0.0.1:9215/api/info http=2 time=0.005547
https://127.0.0.1:9215/api/delay?ms=500 http=2 time=0.506335
wall=0.51 s
=== h2 special responses ===
chunked: 3000
close-delimited: close-delimited response from app-a
HEAD bulk CL: 1048576
204: 204
500: 500
```

两个请求共用一个 TLS 连接，总墙钟约 0.51 s，接近单个 500 ms 请求，而不是 0.5 s + 0.5 s。

### 11.3 h2 请求体与上传

```text
=== h2 POST 2 MiB echo ===
expected=6543a1bc80724494d5d3ac3a609d32885d1a8930e1bc7e1f243bdd658ea9a8ef
got=6543a1bc80724494d5d3ac3a609d32885d1a8930e1bc7e1f243bdd658ea9a8ef
=== h2 PUT upload ===
upload=6543a1bc80724494d5d3ac3a609d32885d1a8930e1bc7e1f243bdd658ea9a8ef
```

2 MiB 请求体完整缓冲后以 Content-Length 转发给 HTTP/1.1 上游；PUT 落盘哈希与客户端一致。

### 11.4 h2 慢客户端背压

4 MiB 下载，`curl --limit-rate 200k`：

```text
=== h2 slow download (4 MiB, limit 200k) ===
wall=20.01 s
4194304 /tmp/h2slow.bin
edgegate_buffered_bytes 0
edgegate_upstream_read_paused_total 13
```

下载耗时 20 s 符合 200 KiB/s 限速；代理应用层缓冲回落到 0，期间发生 13 次“暂停上游读取”，说明 h2 data provider + pending 水位确实把压力传回了上游。

### 11.5 自动化覆盖

```text
[ PASS ] TLS HTTP/1.1
[ PASS ] HTTP/2 basic + POST
[ PASS ] HTTP/2 multiplexed streams
[ PASS ] HTTP/2 chunked/HEAD/204/upload
[ PASS ] HTTP/2 request body limit
[ PASS ] HTTP/2 upstream timeout
[ PASS ] HTTP/2 slow client backpressure
[ PASS ] HTTP/2 retry semantics

integration_test: 26/26 passed
```

集成测试还验证了：h2 单流 8 MiB / 单连接 32 MiB 上限返回 413；上游超时返回 504；GET 在响应前失败可换后端重试，POST 写出后返回 502 且 `retries_total` 不增加。

## 12. 未覆盖/未验证

- h2c（明文 HTTP/2）、HTTP/2 上游、server push、trailer 转发：v1.1 明确不支持；
- 客户端证书/mTLS、SNI 多证书、OCSP stapling、证书热加载：未实现；
- WebSocket/CONNECT：返回 501；
- TLS/HTTP2 吞吐：只有功能与背压验证，没有基准数字；
- IPv6 上游：代码通过 `getaddrinfo` 支持，但没有专门的集成用例；
- 10k+ 并发连接：未压测，`max_clients` 固定 65536，受 ulimit 影响；
- 真实公网 RTT/丢包：只在 loopback 测；背压逻辑与 RTT 无关，但性能数字不适用；
- 管理端认证：没有，默认只监听 loopback；
- 配置热加载：未实现；
- 生产级日志落盘/轮转：未实现。
