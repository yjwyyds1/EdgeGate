# TLS 终止：非阻塞握手、证书与 ALPN

EdgeGate v1.1 在可选的 `tls_listen` 端口上终止 TLS，客户端可以是 HTTP/1.1 或 HTTP/2（ALPN），上游仍是明文 HTTP/1.1。

## 配置

```ini
listen = 0.0.0.0:8080          # 明文端口，可同时保留
tls_listen = 0.0.0.0:8443
tls_cert = certs/cert.pem
tls_key = certs/key.pem
tls_min_version = 1.2          # 1.2 | 1.3
http2 = true                   # ALPN 服务端列表 h2,http/1.1
tls_handshake_timeout_ms = 10000
```

- `tls_listen` 出现才启用 TLS；只配置 `tls_cert/tls_key` 而不配置 `tls_listen` 会报错。
- 证书/私钥必须匹配；加载失败时进程启动失败，不会静默降级。
- `http2=false` 时 ALPN 只提供 `http/1.1`。

生成自签名测试证书：

```bash
bash scripts/gen_test_cert.sh certs
```

该脚本生成 `CN=localhost`、SAN 包含 `localhost` 和 `127.0.0.1` 的 1 年期证书；仅用于本地测试。

## 为什么不能直接 `SSL_read/SSL_write`

非阻塞 TLS 的每次调用都可能返回“现在不能读/写，等对应事件”：

```text
SSL_accept/SSL_read/SSL_write
        │
        ├─ WANT_READ  -> 关注 EPOLLIN
        ├─ WANT_WRITE -> 关注 EPOLLOUT
        ├─ ZERO_RETURN-> EOF
        └─ 其他        -> 错误
```

`TlsConnection` 把 OpenSSL 错误映射为：

```cpp
enum class IoStatus { Ok, WantRead, WantWrite, Eof, Error };
```

`ClientConn` 再把它映射成 epoll 关注：

- 握手阶段：`WantRead → EPOLLIN`，`WantWrite → EPOLLOUT`；握手未完成前不解析 HTTP；
- 读阶段：`SSL_read` 的 `WantWrite` 需要临时关注 `EPOLLOUT`（TLS 1.3 握手后消息/密钥更新）；
- 写阶段：`SSL_write` 的 `WantRead` 需要临时关注 `EPOLLIN`；
- 所有“临时关注”都通过 `io_want_read/io_want_write` 并入 `update_events()`。

## 握手流程

```text
accept()
  -> TLS_new / SSL_set_fd / SSL_set_accept_state
  -> add(EPOLLIN|EPOLLRDHUP)
  -> start(): SSL_accept
       WantRead  -> EPOLLIN
       WantWrite -> EPOLLOUT
       Ok        -> SSL_get0_alpn_selected
                    ├─ "h2"        -> H2Session
                    └─ 其他/空     -> HTTP/1.1 ClientConn
```

握手超时使用 `tls_handshake_timeout_ms`（默认 10 s），由 100 ms tick 检查。

## ALPN

服务端列表：

- `http2=true`：`\x02h2\x08http/1.1`；
- `http2=false`：`\x08http/1.1`。

回调使用 `SSL_select_next_proto`；没有交集时返回 `SSL_TLSEXT_ERR_NOACK`，连接按 HTTP/1.1 处理（客户端仍可正常发 h1 请求）。

验证：

```bash
curl -k --http1.1 -o /dev/null -w '%{http_version}\n' https://127.0.0.1:8443/api/info
# 1.1
curl -k --http2  -o /dev/null -w '%{http_version}\n' https://127.0.0.1:8443/api/info
# 2
openssl s_client -connect 127.0.0.1:8443 -alpn h2 -brief </dev/null
```

## 关闭

`close_now()` 中：

1. 中止在途上游请求；
2. `SSL_shutdown()` 发一次 `close_notify`，不等待对端；
3. `close_later(fd)` 在事件批次结束后关闭 fd。

不等待对端 `close_notify` 是为了避免慢客户端拖住关闭；代价是严格来说不是双向优雅关闭。对于教学代理，这是明确取舍。

## 安全边界

v1.1 只做 TLS 终止，不包含：

- 客户端证书/mTLS；
- SNI 多证书路由；
- OCSP stapling、CRL；
- 证书热加载/自动轮换；
- 上游 TLS（`https://` 后端）；
- HSTS、CSP 等应用层安全头。

生产部署还需要：受信任证书、只开放必要端口、限制 TLS 版本/套件、监控握手失败率、保护管理端口。

## 测试

```bash
sudo apt install -y libssl-dev libnghttp2-dev
./scripts/gen_test_cert.sh certs
./build/edgegate --config config/edgegate-tls.conf

curl -k --http1.1 https://127.0.0.1:8443/api/info
curl -k --http2  https://127.0.0.1:8443/api/info
bash scripts/tls_smoke.sh
```

集成测试会：

- 动态生成自签名证书；
- 验证 TLS HTTP/1.1 与 ALPN h2；
- 验证 h2 GET/POST/上传/下载；
- 验证 h2 多路复用、413、504、背压、重试；
- 构建时没有 nghttp2 则跳过 h2 场景，TLS h1 仍然测试。

## 常见错误

1. **把 `SSL_ERROR_WANT_WRITE` 当错误关闭**：握手会永远失败；必须映射到 `EPOLLOUT`。
2. **握手完成前解析 HTTP**：TLS 记录层数据不是 HTTP 字节流。
3. **`SSL_write` 部分写**：必须用返回值消费缓冲，不能假设一次写完。
4. **直接 `close` 不发 `close_notify`**：对端可能看到截断；本项目发一次但不等待。
5. **证书/私钥不匹配**：`SSL_CTX_check_private_key` 必须在启动时失败。
6. **在 WSL 里用 Windows 代理访问 127.0.0.1**：设置 `no_proxy=*`，否则 curl 可能报 `via 10.255.255.254`。

## 相关源码

- [`include/edgegate/tls.hpp`](../include/edgegate/tls.hpp)、[`src/tls.cpp`](../src/tls.cpp)；
- [`src/proxy.cpp`](../src/proxy.cpp) 的 `ClientConn::handle_tls_handshake()`、`update_events()`、`close_now()`；
- [`tests/integration_test.py`](../tests/integration_test.py) 的 TLS/h2 场景；
- [`scripts/tls_smoke.sh`](../scripts/tls_smoke.sh)、[`scripts/gen_test_cert.sh`](../scripts/gen_test_cert.sh)。
