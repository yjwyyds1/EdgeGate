# HTTP/1.1 转发语义

本文列出 EdgeGate 实际处理的协议行为，以及有意不支持的边界。实现以 [`src/http.cpp`](../src/http.cpp) 和 [`src/proxy.cpp`](../src/proxy.cpp) 为准。

## 请求行

- 方法：RFC token（字母、数字和 `!#$%&'*+-.^_`|~`），最长 32 字节；`CONNECT` 会被识别但返回 501。
- 版本：只接受 `HTTP/1.0` 和 `HTTP/1.1`；其他返回 505。
- 目标：
  - origin-form：`/path?query`；
  - absolute-form：`http://host/path`、`https://host/path`；代理拆出 authority 和 path，仍然用明文 HTTP 访问上游；
  - `*` 仅允许 `OPTIONS`；
  - 其他形式返回 400。
- 请求行最长 8 KiB；超过返回 414。

## 请求头

- 头名必须是 token；头值不允许 CR/LF/NUL，避免响应拆分/请求走私。
- 拒绝 obs-fold（以空格/制表符开头的续行）。
- 最多 200 个头；单头名 ≤ 256 B、单头值 ≤ 8 KiB；总头 ≤ 32 KiB（可配）。
- HTTP/1.1 必须有 Host；多个 Host、空 Host 返回 400。
- `Content-Length`：
  - 支持多个值但必须完全一致；冲突返回 400；
  - 非数字、溢出返回 400。
- `Transfer-Encoding`：
  - 只支持 `chunked`；其他编码返回 501（请求）/502（响应）；
  - `Transfer-Encoding` 与 `Content-Length` 同时出现返回 400，防止走私。
- `Connection` token 决定 keep-alive/close/upgrade；HTTP/1.0 默认 close，`Connection: keep-alive` 可开启。
- `Expect: 100-continue`：代理先向客户端发送 `HTTP/1.1 100 Continue`，然后读 body；向上游转发时删除 `Expect`。
- 其他 `Expect` 值返回 417。
- `Upgrade` 或 `Connection: upgrade` → 501；不做 WebSocket 隧道。
- 请求体上限可使用 `max_request_body_bytes` 限制；0 表示不限制。CL 超限在转发前返回 413；chunked 超限在流式转发中返回 413 并关闭上游。

## 请求体

| 客户端 | 上游 | 行为 |
| --- | --- | --- |
| Content-Length | Content-Length | 流式原样转发，边读边写 |
| chunked | chunked | 解码 chunk，重新编码为 chunk 转发；trailer 丢弃 |
| 无 body | 无 body | 不发送 Content-Length |

代理不会把 chunked 请求体整体缓冲成 Content-Length，因此大文件上传不需要把整个文件放进内存。

## 响应头

- 状态码 100–599；非法状态行返回 502。
- 响应头同样拒绝 obs-fold、CR/LF 注入、TE+CL 冲突。
- 逐跳头过滤：
  - 固定过滤：`Connection`、`Keep-Alive`、`Proxy-Authenticate`、`Proxy-Authorization`、`TE`、`Trailer`、`Transfer-Encoding`、`Upgrade`、`Proxy-Connection`；
  - 还过滤 `Connection` 头中列出的自定义 token。
- 代理追加：
  - `Via: 1.1 edgegate`
  - `X-EdgeGate-Upstream: <backend name>`
  - `X-EdgeGate-Request-Id: <id>`
- 保留 `Set-Cookie` 等多值头；头名大小写不敏感，输出时规范化为 `X-Header-Name` 形式。

## 响应体

| 上游 | HTTP/1.1 客户端 | HTTP/1.0 客户端 |
| --- | --- | --- |
| Content-Length | 原样保留 CL | 保留 CL |
| chunked | 重新 chunked | 去 chunk，关闭定界 |
| 无 CL / 无 TE（close-delimited） | 重新 chunked，客户端可 keep-alive | 去分块，关闭定界 |
| HEAD / 204 / 304 | 无 body；HEAD 保留上游 CL | 同左 |

**为什么重新分帧**：上游的 close-delimited 响应意味着“读到 EOF 才结束”。如果直接把字节流交给客户端并保持连接，客户端无法知道 body 何时结束。代理把 body 重新 chunked，就能在保持字节内容不变的同时让客户端连接继续复用。

## 特殊响应

- `1xx` interim（如 103 Early Hints）：转发给客户端，然后继续解析最终响应；如果代理已经本地回过 100，则丢弃重复的 100。
- `101 Switching Protocols`：转发后关闭两端连接；不支持升级后的协议。
- `HEAD`：不读取响应体，但保留 `Content-Length`。
- `204` / `304`：不读取 body，不发送 `Transfer-Encoding`。
- 上游提前响应（请求体未发完）：停止转发 body，标记上游不可复用，响应结束后关闭客户端。

## keep-alive 与连接池

客户端连接复用条件：

- 请求 `Connection` 不是 close；
- 响应有明确帧边界（CL 或 chunked；close-delimited 会被重新 chunked，因此也可复用）；
- 没有客户端 EOF/写停顿；
- 没有协议升级。

上游连接复用条件：

- 响应 `keep-alive`；
- 不是 close-delimited；
- 响应解析器刚好消费完，`up->in` 为空；
- 请求没有提前中止；
- 不是 101；
- 连接没有在复用前被对端关闭（`MSG_PEEK` 探活）。

## 错误响应

| 状态 | 场景 |
| --- | --- |
| 400 | 请求行/头/CL/TE 非法、缺少 Host、chunk 帧错误 |
| 408 | 请求头或 body 空闲超时 |
| 413 | 请求体超过限制 |
| 414 | 请求行过长 |
| 417 | 不支持的 Expect |
| 431 | 请求头过大 |
| 501 | CONNECT/Upgrade/不支持的 Transfer-Encoding |
| 502 | 上游连接/读写失败、响应解析失败、所有尝试失败 |
| 503 | 没有健康后端、全部 draining |
| 504 | 连接/响应/请求超时 |

错误响应带 `Content-Type: text/plain`、`Content-Length`、`X-EdgeGate-Error` 和 request id；默认关闭连接，避免半截请求污染 keep-alive。

## 明确不支持

- HTTP/2 前端或上游；
- TLS；
- WebSocket/CONNECT/任意协议升级；
- trailer 转发（解析但不发送）；
- 请求/响应压缩（`Content-Encoding` 原样透传）；
- 100-continue 的延迟决策（代理立即回 100，不等上游）；
- 多 worker/多事件循环；
- `Proxy-Protocol`、`Forwarded` 标准头的完整生成（只生成 X-Forwarded-*）。
