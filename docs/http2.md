# HTTP/2 前端：h2 stream → HTTP/1.1 上游

EdgeGate v1.1 在 TLS/ALPN 协商为 `h2` 时启用 nghttp2 server session。上游仍然是明文 HTTP/1.1，因此这是一个**协议转换代理**，不是端到端 HTTP/2。

## 目标与边界

支持：

- 一个 TLS 连接上多个并发 stream；
- GET/HEAD/POST/PUT/PATCH/DELETE；
- 请求/响应头、body；
- chunked/close-delimited 的 HTTP/1.1 上游响应转成 h2 DATA；
- 响应流控与慢客户端背压；
- GET/HEAD 的上游失败重试（请求体已缓冲，可重放）；
- 413（请求体超限）、502/504（上游失败/超时）、HEAD/204/304。

不支持：

- h2c（明文 HTTP/2）；
- HTTP/2 上游；
- server push；
- trailer 转发；
- 流优先级；
- 流式 h2 请求体（v1.1 缓冲请求体）。

## 连接与 stream 模型

```text
TLS connection (ALPN h2)
  │
  ├── H2Session (nghttp2_session)
  │     ├── stream 1 -> H2Stream -> UpstreamConn (HTTP/1.1)
  │     ├── stream 3 -> H2Stream -> UpstreamConn (HTTP/1.1)
  │     └── stream 5 -> H2Stream -> UpstreamConn (HTTP/1.1)
  │
  └── out Buffer -> TLS -> client
```

- `ClientConn` 仍持有 fd、TLS、epoll 注册；
- 握手成功且 ALPN=h2 后创建 `H2Session`，之后 `ClientConn` 把事件委托给 H2Session；
- 每个 `H2Stream` 是一个 `UpstreamOwner`，复用 `Impl::acquire_upstream()`、连接池、健康检查和重试规则。

## nghttp2 回调

| 回调 | 行为 |
| --- | --- |
| `on_begin_headers` | 为 HEADERS 请求创建 `H2Stream`，设置 stream user data |
| `on_header` | 收集 `:method/:path/:authority/:scheme` 和普通头；丢弃 h2 禁止的逐跳头 |
| `on_data_chunk_recv` | 追加请求 body；检查单流/单连接上限 |
| `on_frame_recv` | HEADERS/DATA 带 END_STREAM 时触发 `on_end_stream()` |
| `on_stream_close` | 中止上游、提交 trace、加入回收队列 |

`H2Session::reap()` 在 `mem_recv`/`mem_send` 返回后删除已关闭 stream，避免在 nghttp2 回调里释放仍被引用的对象。

## 请求映射

h2 HEADERS 到达并 END_STREAM 后：

1. 校验 method/path/authority、content-length 与 body 长度；
2. 构造 `RequestHead`：
   - `method`、`path`、`host=authority`、普通 headers；
   - `content_length = body.size()`；
   - `has_content_length`：有 body、或客户端给了 content-length、或 POST/PUT/PATCH；
   - `chunked=false`；
3. 选择后端，取上游连接；
4. 用现有 `build_request_head()` 生成 HTTP/1.1 请求头，接上 body；
5. 发送到上游，解析响应。

**为什么缓冲请求体**：h2 请求体没有天然的长度；要转成 h1，要么用 chunked 流式转发，要么等 END_STREAM 后用 Content-Length。v1.1 选择后者，换取：

- 请求头/响应头映射简单；
- 重试时 body 可重放；
- 不需要把 h2 窗口更新与上游写缓冲做成双向联动。

代价是单流 8 MiB、单连接 32 MiB 上限；超过返回 413。真正的流式 h2 请求体是后续阶段。

## 响应映射

上游 h1 响应头解析后：

- 过滤逐跳头；
- `:status` + 普通头 + `via` + `x-edgegate-upstream` + `x-edgegate-request-id`；
- 无 body（HEAD/204/304）时 `nghttp2_submit_response2(..., nullptr)`；
- 有 body 时注册 `nghttp2_data_provider2`：
  - `pending` 有数据 → 拷贝，返回消费字节数；
  - `pending` 为空且响应未完成 → `NGHTTP2_ERR_DEFERRED`；
  - 上游数据到达 → `nghttp2_session_resume_data`；
  - 响应完成且 pending 为空 → 设置 `NGHTTP2_DATA_FLAG_EOF`。

## 背压

响应方向：

```text
上游 recv -> H2Stream::pending -> nghttp2 data provider -> H2Session::out -> TLS
                    │
        pending >= client_buffer_high ?
                    │
          暂停上游 EPOLLIN
                    │
        provider 消费到 low ?
                    │
          恢复上游 EPOLLIN
```

- h2 自身流控窗口限制单个 stream 的在途 DATA；
- 应用层 `pending` 高/低水位限制代理自身缓冲；
- `H2Session::out` 是 TLS 写缓冲，也计入全局 `buffered_bytes`；
- 集成测试用 `curl --http2 --limit-rate 256k` 下载 1 MiB，确认 `edgegate_upstream_read_paused_total > 0` 且 `buffered_bytes < 2 MiB`。

请求方向：由于请求体是缓冲的，v1.1 不做 h2 窗口级流控；超过上限直接 413。

## 重试

- 请求体完整缓冲，理论上可重放；
- 仍然遵守 h1 的重试规则：非幂等请求写出字节后不重试；收到响应后不重试；达到 `max_attempts` 不重试；
- GET/HEAD 在连接失败/响应前断开时可以换后端重试；
- POST 在连接建立前失败（没有字节写出）可以重试；写出字节后返回 502；
- 集成测试用 flaky 上游验证：h2 GET 重试成功、h2 POST 返回 502 且 `retries_total == 0`。

## 多路复用验证

```bash
curl -k --http2 --parallel \
  -o /tmp/a -o /tmp/b \
  -w '%{url_effective} %{http_version} %{time_total}\n' \
  https://127.0.0.1:8443/api/delay?ms=400 \
  https://127.0.0.1:8443/api/info
```

- 两个请求都输出 `2`；
- 总墙钟约 0.4–0.5 s，而不是 0.4 s + 0.4 s；
- 管理端 `edgegate_client_connections_total` 只增加 1，说明共用一个 TLS 连接。

## 错误处理

| 情况 | 行为 |
| --- | --- |
| 请求体超限 | 413，END_STREAM |
| 上游连接失败、未提交响应头 | 502/504 响应 |
| 上游失败、已提交响应头 | `RST_STREAM(INTERNAL_ERROR)` |
| 上游响应超时 | 504 |
| 客户端 RST/EOF | 中止上游、提交 trace、关闭连接 |
| 请求路径/方法非法 | 400/501 |

## 指标与 trace

每个 h2 stream 都产生一条 `RequestTrace`，与 h1 共用：

- `requests_total` / `responses_total` / `active_requests`；
- `backend_*`；
- `bytes_in`（请求体）/`bytes_out`（响应体）；
- `retry`、`ttfb_ms`、`connect_ms`。

`buffered_bytes` 额外包含 H2Session 的 `out`、每个 stream 的 `pending` 和请求 body。

## 上游 HTTP/2（v1.2）

v1.2 支持把请求转到 **h2c prior-knowledge** 上游：

```ini
[backend app-h2]
url = http://127.0.0.1:9401
http2 = true
```

路径：

```text
客户端 h1 或 h2
    → EdgeGate
        → UpstreamConn（nghttp2 client + connection preface）
            → 上游 h2c
```

实现要点：

- `UpstreamConn` 在 `backend.http2=true` 时建立 nghttp2 **client** session；
- 连接成功后先发 `PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n`，再 SETTINGS；
- `h2_submit_request()` 把 `RequestHead` 映射为 `:method/:path/:authority`；
- 上游响应通过回调组装成合成的 HTTP/1.1 字节流写入 `up->in`，复用现有 `ResponseHeadParser`；
- 当前每个上游连接一次只跑一个 stream，响应结束后不放回 idle 池（实现简单，避免 session 复用状态纠缠）；
- 上游 TLS（`https://`）和真正的多路复用连接池是后续工作。

本地验证：

```bash
./build/eg_h2echo --listen 127.0.0.1:9401 &
# config 中 backend http2=true 指向 9401
./build/edgegate --config ...
curl -s http://127.0.0.1:8080/api/info
```

## WebSocket / CONNECT 隧道（v1.2）

HTTP/1.1 客户端：

- `Upgrade: websocket`：转发握手；收到上游 `101` 后进入双向原始字节隧道；
- `CONNECT host:port`：选中后端并 TCP 连通后，直接回 `200 Connection Established`，再双向隧道（反向代理语义：隧道目标是被选中的 backend，不是任意外网 host）。

演示后端 `GET /api/ws` 完成最小 WebSocket 握手后做原始回显，便于验证隧道。

## 限制与下一步

1. **请求体缓冲**：流式 h2 请求体需要把 h2 DATA、窗口更新、上游写水位联动。
2. **单线程**：所有 stream 共享一个 epoll 事件循环；多 worker 需要重新设计 session 归属。
3. **上游 h2 不复用连接池 / 无上游 TLS**：见上文。
4. **无 push/trailer/优先级**：当前只覆盖普通请求/响应。
5. **无 h2c 前端**：明文 HTTP/2 前端需要 prior knowledge 或 Upgrade，当前前端 h2 只走 ALPN。

## 相关源码

- [`src/h2_impl.inc`](../src/h2_impl.inc)：H2Session、H2Stream、nghttp2 回调、data provider；
- [`src/proxy.cpp`](../src/proxy.cpp)：`UpstreamConn` h2 client、`ClientConn` 隧道、`start_h2()`；
- [`src/tls.cpp`](../src/tls.cpp)：ALPN 与 TLS 传输；
- [`apps/eg_h2echo.cpp`](../apps/eg_h2echo.cpp)：h2c 测试上游；
- [`tests/integration_test.py`](../tests/integration_test.py)：h2 / 隧道场景；
- [`scripts/h2_manual.sh`](../scripts/h2_manual.sh)：手工多路复用/上传/背压脚本。
