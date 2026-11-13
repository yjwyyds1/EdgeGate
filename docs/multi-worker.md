# 多 worker（SO_REUSEPORT）

EdgeGate v1.3 支持 `num_workers > 1`：

```ini
num_workers = 4
listen = 0.0.0.0:8080
admin_listen = 127.0.0.1:9900
```

## 模型

```text
master (父进程)
  ├── worker 0  epoll + listen(SO_REUSEPORT) + admin
  ├── worker 1  epoll + listen(SO_REUSEPORT)
  ├── worker 2  ...
  └── worker N-1
```

- 每个 worker 是完整的单线程事件循环副本（连接池、背压、h2、隧道各自独立）；
- 内核用 `SO_REUSEPORT` 把新连接分到不同 worker；
- **只有 worker 0** 绑定 `admin_listen`（避免端口冲突）；
- 父进程监督子进程：收到 SIGTERM/SIGINT 后向所有 worker 发 SIGTERM；任一 worker 异常退出会牵连其余退出。

## 为什么这样设计

| 取舍 | 原因 |
| --- | --- |
| fork 而不是线程 | 沿用现有无锁状态机，不引入跨线程 Backend/Metrics 共享 |
| 状态不共享 | 健康检查、连接池、trace 每 worker 一份；实现简单，面试可讲“共享 vs 分片” |
| admin 仅 worker 0 | 管理端口唯一；查 metrics 时看到的是 worker 0 的计数，不是全集群总和 |

## 验证

```bash
# 配置 num_workers = 2
./build/edgegate --config config/edgegate.conf
# 日志应有 master supervising 2 workers，以及 worker 0/2、1/2

curl -s http://127.0.0.1:9900/api/status | head -c 200
# 含 "worker_id":0,"num_workers":2,"pid":...

ps -o pid,ppid,cmd -C edgegate
```

集成测试 `multi-worker SO_REUSEPORT` 会起 2 个 worker，打 20 次请求并检查 status。

## 限制与下一步

1. **指标未聚合**：`/metrics` 只反映 worker 0；要全集群需共享内存或 scrape 每个 worker 的独立 admin 端口。
2. **健康状态不共享**：worker A 摘除的后端，worker B 仍可能继续尝试，直到自己也失败。
3. **无热重启 / 平滑升级**：改配置仍需重启整个 master。
4. **TLS 会话票据未跨 worker 共享**。

相关代码：[`apps/edgegate.cpp`](../apps/edgegate.cpp)（fork/监督）、[`src/util.cpp`](../src/util.cpp)（`SO_REUSEPORT`）、[`src/proxy.cpp`](../src/proxy.cpp)（worker 日志与 status 字段）。
