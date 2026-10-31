# EdgeGate 架构与源码导览

> 本文描述 `include/`、`src/`、`apps/`、`tests/` 中**当前可构建的源码**，供阅读、复刻和核对行为使用。项目定位是单机、单事件循环的 HTTP/1.1 教学代理，不是生产级 Nginx 替代品。

## 相关源码

| 文件 | 职责 |
| --- | --- |
| [`include/edgegate/buffer.hpp`](include/edgegate/buffer.hpp) | 连续可读区间 + 尾部可写空间；读指针不搬移，空间不足时压缩/扩容 |
| [`include/edgegate/http.hpp`](include/edgegate/http.hpp)、[`src/http.cpp`](src/http.cpp) | 请求/响应头解析、chunked 解码、逐跳头过滤、序列化、错误响应 |
| [`include/edgegate/config.hpp`](include/edgegate/config.hpp)、[`src/config.cpp`](src/config.cpp) | INI 风格配置、严格校验、尺寸/超时/水位 |
| [`include/edgegate/metrics.hpp`](include/edgegate/metrics.hpp)、[`src/metrics.cpp`](src/metrics.cpp) | 原子计数、Gauge、固定桶直方图、Prometheus 文本 |
| [`include/edgegate/backend.hpp`](include/edgegate/backend.hpp)、[`src/backend.cpp`](src/backend.cpp) | 后端健康状态机、冷却、快照、round-robin/least-conn |
| [`include/edgegate/retry.hpp`](include/edgegate/retry.hpp)、[`src/retry.cpp`](src/retry.cpp) | 纯函数重试判定 |
| [`include/edgegate/event_loop.hpp`](include/edgegate/event_loop.hpp) | epoll 注册、事件分发、延迟关闭 fd |
| [`include/edgegate/proxy.hpp`](include/edgegate/proxy.hpp)、[`src/proxy.cpp`](src/proxy.cpp) | `ProxyServer` 实现、客户端/上游连接状态机、连接池、超时、trace |
| [`include/edgegate/admin.hpp`](include/edgegate/admin.hpp)、[`src/admin.cpp`](src/admin.cpp) | 管理端口、HTML 面板、JSON/指标/追踪路由 |
| [`include/edgegate/log.hpp`](include/edgegate/log.hpp)、[`src/log.cpp`](src/log.cpp) | 分级日志 |
| [`include/edgegate/util.hpp`](include/edgegate/util.hpp)、[`src/util.cpp`](src/util.cpp) | 时间、字符串、socket、地址解析 |
| [`apps/edgegate.cpp`](apps/edgegate.cpp) | 命令行入口、配置校验、启动 |
| [`apps/demo_backend.cpp`](apps/demo_backend.cpp) | 真实文件/API 演示后端 |
| [`apps/eg_loadgen.cpp`](apps/eg_loadgen.cpp) | keep-alive 压测器 |
| [`tests/`](tests/) | 6 个单元测试 + 18 个端到端场景 |

## 1. 目的与交付边界

EdgeGate 接收 HTTP/1.0/1.1 客户端请求，按 `round_robin` 或 `least_conn` 选择一个后端，使用非阻塞 socket 转发请求头和请求体，再把响应头和响应体流式转发给客户端。它维护每后端连接池、主动/被动健康检查、超时、限流式的缓冲水位和可观测性。

**不包含**：TLS、HTTP/2、WebSocket/CONNECT、多 worker、配置热加载、管理端认证、缓存、动态服务发现。协议升级请求会得到 501，而不是被“透明转发”。

## 2. 核心组件与数据流

```mermaid
flowchart LR
    subgraph EdgeGate process
        EL[EventLoop: epoll]
        CC[ClientConn state machine]
        UC[UpstreamConn state machine]
        PP[RequestHeadParser / ResponseHeadParser]
        CD[ChunkedDecoder]
        POOL[Per-backend idle pool]
        BAL[Balancer]
        HC[Health checker]
        M[Metrics + trace ring]
        AD[AdminServer]
    end
    C[Client] --> CC
    CC --> PP
    CC --> CD
    CC --> BAL
    BAL --> POOL
    CC <--> UC
    UC --> CD
    HC --> Backends
    CC --> M
    UC --> M
    AD --> M
    EL --> CC
    EL --> UC
    EL --> AD
    EL --> HC
```

| 层 | 拥有什么状态 | 对外提供什么 |
| --- | --- | --- |
| `EventLoop` | epoll fd、fd→handler 映射、延迟关闭列表 | `add/mod/remove/close_later/run_once` |
| `ClientConn` | 客户端 fd、in/out Buffer、请求解析器、当前请求上下文、trace | 读请求、转发、重试、错误响应、超时 |
| `UpstreamConn` | 上游 fd、in/out Buffer、连接/空闲标志 | 非阻塞 connect、部分写、事件转发给 owner |
| `Backend` | 健康/冷去/draining、计数、延迟直方图 | `selectable`、`record_success/failure`、snapshot |
| `Balancer` | 轮询游标 | 选择下一个可用后端（可排除） |
| `ProxyServer::Impl` | 所有连接、池、指标、trace ring、管理端 | 启动/停止、状态 JSON、Prometheus |

## 3. 事件循环与 fd 生命周期

`EventLoop` 是单线程 epoll 封装：

- `add(fd, events, handler)`：`EPOLL_CTL_ADD`，并记录 `handlers_[fd]`；
- `mod(fd, events)`：只更新关注事件；
- `remove(fd)`：从 epoll 摘除但**不关闭** fd，用于把上游连接放回池；
- `close_later(fd)`：摘除并在本批事件处理完后 `close`，避免回调中途 fd 号被复用；
- `run_once(timeout_ms)`：`epoll_wait`，逐个按 fd 查表分发；批次结束后统一关闭延迟关闭列表。

**为什么延迟关闭**：一次 `epoll_wait` 可能返回多个 fd；如果处理 A 时关闭了 B，而 B 的事件还在同一批里，B 的 handler 指针可能悬垂。延迟关闭 + 按 fd 查表使“已删除的 fd 不会在本批再次分发”，同时避免 fd 号在批次内被 accept 复用。

## 4. 缓冲与背压不变量

`Buffer` 维护 `r_`（读指针）和 `w_`（写指针）：

- `consume(n)` 只前进 `r_`，不搬移内存，因为调用方可能持有指向已读区间的 `string_view`；
- `reserve(n)` 在空间不足时先压缩已读前缀，再按 1.5x/2x 扩容；
- `view()` 只在下一次 `append/reserve` 前有效。

代理侧有三类水位：

| 缓冲 | 高水位后的动作 | 低水位后的动作 |
| --- | --- | --- |
| 客户端 out（响应） | 禁用上游 `EPOLLIN` | 恢复上游 `EPOLLIN` |
| 上游 out（请求体） | 禁用客户端 `EPOLLIN` | 恢复客户端 `EPOLLIN` |
| 全局 in+out 总和 | 所有连接进入全局背压 | 降到 75% 后解除 |

不变量：

1. 任何一次 `recv` 最多 64 KiB，因此实际缓冲 ≤ 高水位 + 64 KiB（单连接）；
2. `out` 非空时必须关注 `EPOLLOUT`；`out` 为空时取消；
3. 慢客户端超过 `client_write_stall_ms` 没有写进展会被关闭；
4. 连接池空闲连接不注册 epoll，不占用事件循环 CPU。

## 5. 客户端状态机

`ClientConn::State`：

```text
ReadRequest ──头完整──> Dispatching ──取到连接──> Connecting ──连接完成──> SendingBody
     ^                       │                         │                         │
     │                       └──无可用后端──> Draining/Closing                  │
     │                                                                         v
     └──────────────────────── 响应完成 <── StreamingResponse <── WaitingResponse
```

关键转换：

- `ReadRequest`：读客户端，增量解析请求头；解析错误直接返回 4xx/431/505 并关闭；
- `Dispatching`：暂停客户端读（避免在连接建立期间无限缓冲 body），选择后端；
- `Connecting`：等待上游 `EPOLLOUT`，检查 `SO_ERROR`；
- `SendingBody`：把 CL/chunked body 流式写入上游 out；上游 out 高水位时暂停客户端读；
- `WaitingResponse`：请求体已入队，等待上游响应头；
- `StreamingResponse`：解析响应头/体，写客户端 out；客户端 out 高水位时暂停上游读；
- `Draining`：响应已生成但 out 未排空；排空后回到 `ReadRequest` 或关闭。

**提前响应**：如果上游在请求体尚未发完时返回响应（例如 413），代理会停止转发 body、清空上游 out、标记连接不可复用，并在响应结束后关闭客户端连接。这样不会把“半截请求”误认为完整请求。

## 6. 请求/响应解析

### 6.1 请求头

`RequestHeadParser::parse()`：

1. 在 Buffer 中找 `\r\n\r\n`，超过 `max_request_header_bytes` 返回 431；
2. 解析请求行：方法、target、版本；只接受 HTTP/1.0/1.1；
3. absolute-form 目标拆出 authority 和 origin-form path；
4. 逐行解析头：拒绝 obs-fold、非法 token、控制字符；
5. 校验 Host 数量、Content-Length 冲突、Transfer-Encoding 与 Content-Length 共存；
6. 解析 `Connection` token、`Expect: 100-continue`、`Upgrade`；
7. 消费整个头部，body 留在 Buffer 中，由 `BodyMode` 决定如何读取。

限制：头部 32 KiB、200 个头、单头名 256 B、单头值 8 KiB、请求行 8 KiB。

### 6.2 chunked

`ChunkedDecoder` 是独立状态机：

```text
SizeLine -> Data -> DataCrlf -> SizeLine
   |                              |
   +-- size==0 --> TrailerLine --> Done
```

- 支持 chunk extension（丢弃）；
- chunk 上限 256 MiB，行长上限 8 KiB；
- trailer 保存下来供调用方使用（代理当前不转发 trailer，直接发送 `0\r\n\r\n`）；
- 返回 Data 时消费输入 Buffer，并返回指向缓冲区的 view；调用方必须在下一次 append/reserve 前复制。

### 6.3 响应

`ResponseHeadParser::parse(in, request_is_head, ...)` 根据请求方法和状态码决定 body 模式：

- HEAD / 1xx / 204 / 304 → 无 body；
- `Transfer-Encoding: chunked` → chunked；
- `Content-Length` → 定长；
- 都没有 → 读到连接关闭（HTTP/1.0 或 `Connection: close`）。

代理对 HTTP/1.1 客户端会把 chunked/close-delimited 响应重新分帧为 chunked，因此客户端连接仍可 keep-alive；HTTP/1.0 客户端则去分块并用关闭连接定界。

## 7. 连接池

池按 `Backend*` 维护 `std::deque<UpstreamConn*>`：

- `acquire_upstream()`：
  - 取 idle 队首，先用 `recv(MSG_PEEK|MSG_DONTWAIT)` 检查连接；
  - 返回 0（EOF）、>0（脏数据）或其他错误 → 丢弃；
  - 否则清空 in/out、设为 assigned、增加 active 计数、重新注册 epoll。
- 无 idle 且 `active + idle < max_connections` → 新建非阻塞连接；
- 达到上限 → 返回 `pool_full`，调用方换后端；
- `release_upstream(up, reusable)`：
  - 可复用且 idle 未满 → `loop.remove(fd)`，放回队列，增加 idle 计数；
  - 否则 `destroy_upstream()` 关闭。
- `sweep_idle()` 每 100 ms 扫描队首，关闭超过 `backend_idle_timeout_ms` 的连接。

**复用条件**：上游响应 `keep-alive`、响应有明确帧边界（不是 close-delimited）、没有 101、请求没有提前中止、`up->in` 为空、没有关闭标记。

## 8. 负载均衡与健康

`Backend` 状态：

```text
healthy=true, failures=0, successes=0
  record_failure -> failures++ ; failures>=threshold -> healthy=false
  record_success -> failures=0 ; successes++ ; if !healthy && successes>=threshold -> healthy=true
  connect_failure -> cooldown_until = now + cooldown_ms
selectable(now) = healthy && !draining && now >= cooldown_until
```

- `round_robin`：从游标开始找第一个 selectable 且不在 exclude 的后端；
- `least_conn`：选择 `active_connections` 最小者，平局按游标顺序；
- 主动健康检查每次只允许一个在途连接（`health_check_in_progress`），超时/失败计入被动失败；
- 管理端 drain 只影响 `selectable`，不会杀死已有连接；enable 立即恢复（若健康）。

## 9. 重试

`can_retry(RetryInput)` 是纯函数，输入包括方法、已尝试次数、是否写出请求字节、是否已转发 body、body 是否完整、是否可重放、是否已收到响应。

```cpp
if (response_started) return false;
if (attempts_done >= max_attempts) return false;
if (!idempotent && idempotent_only && request_bytes_written) return false;
if (body_forwarded && !(body_complete && body_replayable)) return false;
return true;
```

`ClientConn::on_upstream_failure()` 在失败后：

- 标记后端失败/冷却；
- 如果尚未收到响应且 `can_retry()`，重置响应解析状态，重新选择后端；
- 如果已收到部分响应，直接关闭客户端（不能拼接两段响应）；
- 否则返回 502/504。

body 重放缓冲只保存“已转发的解码后字节”，上限 `retry_body_buffer_bytes`；超出后 `replay_ok=false`，后续失败不再重试。

## 10. 可观测性与管理端

`Metrics` 使用原子计数和固定桶直方图（1/2/5/10/25/50/100/250/500/1000/2500/5000 ms + Inf）。单线程下原子不是必须的，但为未来多 worker 预留。

`RequestTrace` 环形缓冲 256 条，记录：

- request id、客户端 IP、方法、路径；
- 最终后端、attempts、是否重试；
- 状态码、错误、duration/ttfb/connect；
- 请求/响应字节数、chunked 标志、keep-alive；
- 起始墙钟时间。

`AdminServer` 是独立 fd/监听端口：

- 接受连接后读一个请求头；
- 路由 `/`、`/healthz`、`/api/status`、`/api/requests`、`/api/requests/<id>`、`/metrics`、`/api/backends/<name>/drain|enable`；
- 响应固定 `Connection: close`，不参与代理转发；
- 管理连接 30 秒无响应会被回收。

## 11. 构建与验证入口

`CMakeLists.txt`：

- `edgegate_core`：util/log/http/config/metrics/backend/retry/admin/proxy；
- `edgegate`：主程序；
- `eg_demo`：演示后端（链接 libcrypto）；
- `eg_loadgen`：压测器；
- CTest：`buffer_test`、`http_test`、`config_test`、`balancer_test`、`retry_test`、`metrics_test`、`integration_test`。

集成测试用 Python 标准库启动真实进程和原始 socket，不依赖 curl/ab；覆盖真实文件上传/下载哈希、chunked、keep-alive、故障转移、重试和慢客户端背压。

## 12. 继续实现时的边界

1. **先复现，再重写**：先用集成测试确认当前行为，再按状态机和解析器接口自己实现。
2. **TLS/HTTP2 不是加一个库就完事**：需要 ALPN、连接生命周期、HTTP/2 流多路复用、流控与 HTTP/1.1 上游的映射。
3. **多 worker 需要重新设计状态**：后端健康、连接池、指标、trace ring 都要么分片要么共享；不能直接把单线程代码复制到多线程。
4. **重试需要业务幂等键**：当前只按 HTTP 方法；生产系统常需要 `Idempotency-Key` 或业务去重表。
5. **背压只覆盖应用层**：内核 socket 缓冲仍可能吸收数据；真实高延迟链路上需要更细的窗口/速率控制。
6. **管理端安全**：没有认证；如果监听非 loopback，必须先加认证、mTLS 或反向代理保护。
