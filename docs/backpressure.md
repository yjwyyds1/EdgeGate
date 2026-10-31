# 背压：慢客户端时不要无限读上游

## 问题

假设后端能以 1 GB/s 发送响应，客户端每秒只读 10 KB。如果代理每收到上游可读事件就 `recv` 并把数据追加到客户端输出缓冲，那么：

- 客户端输出缓冲会持续增长；
- 进程 RSS 随连接数线性上涨；
- 最终 OOM 或被内核 OOM killer 杀死。

正确做法不是“读一点就 sleep”，而是**让事件循环停止关注上游的可读事件**，把“客户端消费速度”反馈给上游。

## 实现

### 双向水位

每个客户端连接有：

```text
client_buffer_high  (默认 256 KiB)
client_buffer_low   (默认  64 KiB)
```

每个上游连接有：

```text
upstream_buffer_high (默认 256 KiB)
upstream_buffer_low  (默认  64 KiB)
```

响应方向：

```text
upstream readable
      |
      v
  recv 64 KiB --> up->in --> 解析/分帧 --> client out
                                              |
                              out.size() >= high ?
                                       /        \
                                     是          否
                                     |            |
                          禁用 EPOLLIN(上游)   继续读
                                     |
                          客户端 EPOLLOUT 排空
                                     |
                              out.size() < low ?
                                     |
                              恢复 EPOLLIN(上游)
```

请求方向同理：上游 out 堆积到高水位时，禁用客户端 `EPOLLIN`，等上游 out 降到低水位后再继续读客户端 body。

### 为什么高水位不是硬上限

每次 `recv` 最多 64 KiB；如果读之前 out 是 250 KiB、高水位 256 KiB，读完后可能是 314 KiB。因此：

- 高水位是“软阈值”，用于尽快停止继续读；
- 单连接实际缓冲 ≤ `high + 64 KiB`；
- 如果业务不能接受这个 overshoot，应把高水位调低或减小单次 `recv` 大小。

### 全局预算

单连接有界不代表总内存有界：10,000 个慢客户端 × 320 KiB ≈ 3 GiB。`max_total_buffered_bytes` 给所有连接的 `in + out` 总和设上限：

- 超过预算 → `global_backpressure=true`；
- 每个客户端在 out 大于低水位时暂停上游读；
- 总缓冲降到预算的 75% 以下 → 解除。

全局预算不是精确的分配器，而是最后一道保险；真正限制单连接的是水位。

### 慢客户端超时

如果客户端 out 一直非空且 `client_write_stall_ms` 内没有任何写进展，代理直接关闭连接：

- 避免“客户端建立了连接但永不读”；
- 关闭后上游连接被销毁（不回到池），避免半截响应污染复用。

### 事件关注

`ClientConn::update_events()`：

```text
读客户端 = (state == ReadRequest 且 out 为空)
        或 (state == SendingBody 且 body 未完成)
        且 未被暂停
写客户端 = out 非空
```

`UpstreamConn::update_events()`：

```text
读上游 = 已连接且未被暂停
写上游 = connecting 或 out 非空
```

只有关注集合变化时才 `epoll_ctl(MOD)`，避免每个事件都做一次系统调用。

## 实测

集成测试中的慢客户端场景：

- 响应 `/api/bulk?mb=4`，4 MiB；
- 客户端每次读 16 KiB，然后 sleep 20 ms；
- 同时每 200 ms 查询 `/metrics`。

结果（一次实际运行）：

```text
bp: received=798720/4194304 reads=50 buffered=262144 pauses=11 resumes=10
bp: received=1593344/4194304 reads=100 buffered=262144 pauses=14 resumes=13
bp: received=2387968/4194304 reads=150 buffered=0 pauses=15 resumes=15
backpressure: buffered<=262144B pauses=15 rss=5652KiB
```

另一次运行：

```text
backpressure: buffered<=262144B pauses=30 rss=7932KiB
```

说明：

- 应用层缓冲峰值正好是高水位 256 KiB；
- 确实发生了多次“暂停上游读 → 客户端消费 → 恢复”；
- 进程 RSS 只有约 5.6–7.9 MiB，没有随 4 MiB 响应线性增长到几十 MiB。

手动复现：

```bash
# 终端 1：启动 demo 与 edgegate
./build/edgegate --config config/edgegate.conf

# 终端 2：慢速读取 8 MiB
python3 scripts/slow_client.py --port 8080 --admin 9900 --mb 8
```

## 常见错误

1. **只限制 out 大小，不停止读上游**：到了上限后继续读，然后丢弃数据——响应就坏了。
2. **暂停客户端读后忘记恢复**：连接会永久卡住；需要在上游 out 排空时恢复。
3. **在暂停期间还处理 EPOLLIN**：`epoll` 是水平触发，必须真正 `MOD` 掉 `EPOLLIN`，否则事件会反复到来。
4. **把背压和超时混为一谈**：背压是正常流控；超时是异常清理。
5. **忽略内核 socket 缓冲**：应用层 out 很小时，内核仍可能缓存几百 KB；这不代表应用层没有背压，测试要同时看 `/metrics` 和 RSS。
6. **全局预算没有滞回**：在阈值附近反复开关会造成抖动；本项目在 75% 处解除。

## 相关源码

- [`src/proxy.cpp`](../src/proxy.cpp)：`write_client_body()`、`on_upstream_readable()`、`set_upstream_paused()`、`update_global_gauges()`；
- [`include/edgegate/buffer.hpp`](../include/edgegate/buffer.hpp)；
- [`tests/integration_test.py`](../tests/integration_test.py) 的 `test_backpressure`。
