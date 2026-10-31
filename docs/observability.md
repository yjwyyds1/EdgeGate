# 可观测性：指标、trace 与管理面板

EdgeGate 的目标之一是“代理做了什么”可以被直接看到，而不是只能猜。所有状态通过管理端口暴露，默认 `127.0.0.1:9900`。

## 端点

| 路径 | 内容 |
| --- | --- |
| `/` | HTML 面板，2 秒刷新，展示后端、最近请求、缓冲、P95 |
| `/healthz` | `{"status":"ok"}` |
| `/api/status` | 完整 JSON：配置摘要、全局指标、后端快照、最近 20 条请求 |
| `/api/requests?limit=N` | 最近 N 条请求 trace（N ≤ 100） |
| `/api/requests/<id>` | 按 request id 查单条 trace |
| `/metrics` | Prometheus 文本格式 |
| `POST /api/backends/<name>/drain` | 停止向该后端派发新请求 |
| `POST /api/backends/<name>/enable` | 恢复派发 |

管理端没有认证，默认只绑定 loopback；配置成非 loopback 会在启动日志中警告。

## Prometheus 指标

### 全局

| 指标 | 类型 | 含义 |
| --- | --- | --- |
| `edgegate_uptime_seconds` | gauge | 进程运行秒数 |
| `edgegate_requests_total` | counter | 进入代理的请求数 |
| `edgegate_responses_total` | counter | 完成响应的请求数 |
| `edgegate_client_connections_total` | counter | 接受的客户端连接数 |
| `edgegate_client_disconnects_total` | counter | 关闭的客户端连接数 |
| `edgegate_upstream_connections_total` | counter | 上游尝试次数（含池复用） |
| `edgegate_requests_rejected_total` | counter | 错误响应数 |
| `edgegate_active_clients` | gauge | 当前客户端连接 |
| `edgegate_active_requests` | gauge | 当前在途请求 |
| `edgegate_buffered_bytes` | gauge | 所有连接 in+out 的应用层缓冲 |
| `edgegate_healthy_backends` | gauge | 健康后端数 |
| `edgegate_retries_total` | counter | 重试次数 |
| `edgegate_bytes_in_total` | counter | 转发的请求体字节 |
| `edgegate_bytes_out_total` | counter | 转发的响应体字节 |
| `edgegate_chunked_requests_total` | counter | chunked 请求数 |
| `edgegate_chunked_responses_total` | counter | chunked 响应数 |
| `edgegate_http10_requests_total` | counter | HTTP/1.0 请求数 |
| `edgegate_upstream_read_paused_total` | counter | 因客户端慢而暂停上游读的次数 |
| `edgegate_upstream_read_resumed_total` | counter | 恢复上游读的次数 |
| `edgegate_client_read_paused_total` | counter | 因上游写慢而暂停客户端读的次数 |
| `edgegate_client_read_resumed_total` | counter | 恢复客户端读的次数 |
| `edgegate_health_checks_total` | counter | 主动健康检查次数 |
| `edgegate_health_failures_total` | counter | 主动健康检查失败次数 |

### 错误

```text
edgegate_errors_total{reason="parse|connect|read|write|timeout|no_backend|body_too_large|upstream_eof"}
```

### 延迟直方图

```text
edgegate_request_duration_ms_bucket{le="1|2|5|10|25|50|100|250|500|1000|2500|5000|+Inf"}
edgegate_request_duration_ms_sum
edgegate_request_duration_ms_count
edgegate_ttfb_ms_...
edgegate_upstream_connect_ms_...
```

固定桶的好处是无锁、可预测；分位数是桶上界估计，不是精确值。`docs/benchmark.md` 中的 P50/P95/P99 来自压测器原始延迟样本，不是这里的桶估计。

### 后端标签

```text
edgegate_backend_healthy{backend="app-a"}
edgegate_backend_draining{backend="app-a"}
edgegate_backend_active_connections{backend="app-a"}
edgegate_backend_idle_connections{backend="app-a"}
edgegate_backend_requests_total{backend="app-a"}
edgegate_backend_errors_total{backend="app-a"}
edgegate_backend_latency_ms{backend="app-a",quantile="0.5|0.95|0.99"}
```

## 请求 trace

每个请求生成 16 位十六进制 ID；客户端可以提供 `X-Request-Id`，但只接受 `[A-Za-z0-9._-]` 且 ≤ 64 字符，否则代理生成自己的 ID。响应头返回 `X-EdgeGate-Request-Id`。

trace 字段：

```json
{
  "id": "eg-9fcb624b5d1ae554",
  "method": "GET",
  "path": "/files/blob.bin",
  "client": "127.0.0.1",
  "status": 200,
  "backend": "app-b",
  "attempts": 1,
  "duration_ms": 1.234,
  "ttfb_ms": 0.812,
  "connect_ms": 0.201,
  "bytes_in": 0,
  "bytes_out": 262144,
  "keep_alive": true,
  "chunked_request": false,
  "chunked_response": false,
  "retried": false,
  "started_unix_ms": 1790000000000,
  "error": ""
}
```

查询：

```bash
curl -s http://127.0.0.1:9900/api/requests/eg-9fcb624b5d1ae554 | python3 -m json.tool
```

trace 环形缓冲只有 256 条；它是诊断工具，不是审计日志。长期审计应使用 JSONL access log。

## 访问日志

`access_log` 为路径时每请求写一行 JSON；`-` 写 stderr；`off` 关闭。示例：

```json
{"ts":"2026-10-30T07:50:06.193Z","request_id":"eg-e352fe9480cf222e","client":"127.0.0.1","method":"PUT","path":"/uploads/up.bin","status":200,"upstream":"app-a","attempts":1,"duration_ms":14.000,"ttfb_ms":14.000,"bytes_in":1048576,"bytes_out":128,"retried":false,"error":""}
```

注意：访问日志是同步写；高 QPS 下会拖慢事件循环。生产应改为异步队列/批量写。

## 管理面板

面板用原生 JS 每 2 秒 `fetch('/api/status')`，展示：

- 请求总数、活跃客户端/请求、缓冲占用、P95、健康后端；
- 后端表：healthy/draining、active/idle、请求、错误、P95；
- 最近请求表：时间、方法、路径、状态、后端、耗时、attempts、request id 链接。

没有外部 CDN 依赖，内网/离线可用。

## Prometheus 抓取示例

```yaml
scrape_configs:
  - job_name: edgegate
    static_configs:
      - targets: ["127.0.0.1:9900"]
```

建议告警：

- `edgegate_healthy_backends == 0` 持续 30 s；
- `edgegate_errors_total{reason="connect"}` 速率突增；
- `edgegate_buffered_bytes` 接近 `max_total_buffered_bytes`；
- `edgegate_upstream_read_paused_total` 持续增长但吞吐不升（可能是慢消费者）；
- `rate(edgegate_requests_total[1m])` 与 `responses_total` 差距扩大（请求泄漏/上游卡住）。

## 相关源码

- [`src/metrics.cpp`](../src/metrics.cpp)：计数器、直方图、Prometheus 渲染；
- [`src/proxy.cpp`](../src/proxy.cpp)：trace ring、access log、`status_json()`；
- [`src/admin.cpp`](../src/admin.cpp)：路由和面板；
- [`docs/benchmark.md`](benchmark.md)：如何用这些指标做对照实验。
