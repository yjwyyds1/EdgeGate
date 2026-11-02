# EdgeGate 实战教程：从零构建 C++20 HTTP/1.1 + HTTP/2 反向代理

> 面向想投递 Linux 网络、后端基础设施、C++ 服务端岗位的读者。假设你会 C++ 基础（类、RAII、STL、CMake），但没写过代理。
>
> 建议节奏：**5–7 周，每天 2–4 小时**。原则：**先自己想、自己写，卡住再看参考实现**。直接抄完代码等于什么都没学到。
>
> 当前源码对应 v1.1，包含 HTTP/1.1、TLS 终止和 HTTP/2 前端（h2 → h1）。README 描述的是已实现能力；本教程既讲“怎么复刻”，也讲“为什么这样设计”和“面试怎么讲”。

---

## 当前源码的 WSL 复现路线

> 想从空目录一路做到 GitHub（分阶段实现、每阶段构建/测试、分阶段 commit、SSH remote、fresh clone 验证），按 [docs/wsl-from-scratch.md](docs/wsl-from-scratch.md) 操作；本节是最短复现路径。

> 本机环境（2026-10-30 核对）：Windows 11 + WSL2 Ubuntu，4 vCPU，GCC 15.2.0，CMake 4.2.3，Ninja。不要照旧教程重装系统。

### A. 确认环境

在 PowerShell：

```powershell
wsl --list --verbose
```

进入 Ubuntu 后：

```bash
cat /etc/os-release | head -n 4
c++ --version | head -1
cmake --version | head -1
nproc
```

需要 GCC 13+、CMake 3.22+。TLS 需要 OpenSSL 开发包，HTTP/2 需要 nghttp2：

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build libssl-dev libnghttp2-dev curl
```

没有 `libnghttp2-dev` 时 TLS/HTTP1.1 仍可构建，但 ALPN 只提供 `http/1.1`，HTTP/2 场景会被跳过。

### B. 复制到 WSL 自己的文件系统

不要直接在 `/mnt/d` 下跑基准；跨文件系统 IO 会干扰数字。源码可以先从 Windows 挂载路径复制：

```bash
mkdir -p "$HOME/dev"
rsync -a --exclude='/build/' --exclude='/build-*/' \
  /mnt/d/project/EdgeGate/ "$HOME/dev/EdgeGate/"
cd "$HOME/dev/EdgeGate"
cmake -S . -B build-wsl -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-wsl --parallel
ctest --test-dir build-wsl --output-on-failure
```

预期 `7/7 tests passed`（6 个单元测试 + 集成测试）；集成测试内部有 26 个场景（HTTP/1.1 + TLS + HTTP/2）。如果显示 `No tests were found`，先确认 `pwd`、`-DBUILD_TESTING=ON` 和 CMake 配置成功。

### C. 手工跑通三个真实场景

```bash
mkdir -p /tmp/egdemo/root /tmp/egdemo/uploads
echo "hello edgegate" > /tmp/egdemo/root/hello.txt

./build-wsl/eg_demo --listen 127.0.0.1:9001 --name app-a \
  --root /tmp/egdemo/root --uploads /tmp/egdemo/uploads &
./build-wsl/eg_demo --listen 127.0.0.1:9002 --name app-b \
  --root /tmp/egdemo/root --uploads /tmp/egdemo/uploads &
./build-wsl/eg_demo --listen 127.0.0.1:9003 --name app-c \
  --root /tmp/egdemo/root --uploads /tmp/egdemo/uploads &

./build-wsl/edgegate --config config/edgegate.conf
```

另开终端：

```bash
curl -i http://127.0.0.1:8080/api/info
curl -s http://127.0.0.1:9900/api/status | head -c 300
```

应看到 `Via: 1.1 edgegate`、`X-EdgeGate-Upstream`、`X-EdgeGate-Request-Id`，重复请求会在 app-a/b/c 之间分布。

TLS 与 HTTP/2（先停掉上面的明文代理，因为示例配置同样监听 8080/9900）：

```bash
bash scripts/gen_test_cert.sh certs
./build-wsl/edgegate --config config/edgegate-tls.conf
```

另开终端：

```bash
export no_proxy='*' NO_PROXY='*'
curl -k --http1.1 -o /dev/null -w '%{http_version}\n' https://127.0.0.1:8443/api/info
# 1.1
curl -k --http2 -o /dev/null -w '%{http_version}\n' https://127.0.0.1:8443/api/info
# 2
curl -k --http2 --parallel \
  -o /tmp/a -o /tmp/b \
  https://127.0.0.1:8443/api/delay?ms=400 https://127.0.0.1:8443/api/info
```

如果 curl 报 `via 10.255.255.254`，是 WSL 把 `http_proxy` 也用于 127.0.0.1；`no_proxy=*` 可解决。

### D. 自己复刻的目录

```bash
lab_dir="$(mktemp -d "$HOME/dev/EdgeGate-lab.XXXXXX")"
cd "$lab_dir"
```

建议按本教程章节实现；每完成一章，把该章的测试场景写成自己的测试，不要直接覆盖参考实现。

### E. 失败时先检查

| 现象 | 检查点 |
| --- | --- |
| CMake 报 Windows 不支持 | 你在 PowerShell/cmake，不在 WSL |
| 连接 9001 失败 | 三个 `eg_demo` 是否真的在跑；端口是否被旧进程占用 |
| 8080 无法访问 | `edgegate` 是否启动；`listen` 是否 0.0.0.0/127.0.0.1 |
| 502/504 | 看管理端 `/api/status` 的 backend 状态和 trace |
| 集成测试端口冲突 | `pkill -x edgegate; pkill -x eg_demo` 后重跑 |
| 上传/下载哈希不一致 | 是否经过代理；是否命中了不同实例的 uploads 目录（演示后端要共享目录） |

---

## 第 0 章 这个项目适合什么岗位，什么时候该换

### 0.1 适合

- Linux 网络/网关/接入层：epoll、非阻塞 IO、连接生命周期、背压；
- C++ 服务端/基础设施：状态机、内存边界、协议解析、可观测性；
- SRE/平台工程：健康检查、故障转移、运维面板、指标。

### 0.2 不适合

- 你想做“高并发百万连接”的旗舰项目：单事件循环不够，需要多 reactor/多 worker，范围会膨胀；
- 你想做算法/量化：MiniExchange 更合适；
- 你只想调库：用 Nginx/Envoy 就行，不需要自己写代理。

### 0.3 和简历的关系

这个项目的价值不在“能转发 HTTP”，而在你能解释：

- 为什么慢客户端会让代理 OOM，水位怎么设计；
- 为什么 POST 不能随便重试；
- chunked 怎么解码/重新分帧，keep-alive 怎么判断；
- 连接池什么条件下复用，什么条件下必须关闭；
- 单线程代理的吞吐上限在哪里，怎么测。

---

## 第 1 章 环境与最小可运行代理

### 1.1 先画状态机，再写代码

在写任何 socket 代码之前，先回答：

1. 客户端连接可能处于哪些状态？
2. 上游连接可能处于哪些状态？
3. 一次请求从读到响应要经过哪些事件？
4. 哪些操作会改变 epoll 关注的事件？
5. 哪些缓冲必须有限制？

EdgeGate 的答案在 [`ARCHITECTURE.md`](ARCHITECTURE.md) 第 5 节。你的答案不必一样，但必须自洽。

### 1.2 最小骨架

建议顺序：

1. `util`：非阻塞 socket、`create_listener`、`connect_nonblocking`、`peer_ip`；
2. `Buffer`：连续可读区间 + 可写尾部；
3. `EventLoop`：epoll add/mod/remove/close_later/run_once；
4. 一个只会返回 `200 OK` 的监听器；
5. 再接 HTTP 解析和上游转发。

不要一开始就写连接池、健康检查、管理面板。先让“一个请求进来、一个响应出去”跑通。

### 1.3 第一个里程碑的验收

- `curl -v http://127.0.0.1:8080/` 能看到完整响应；
- 客户端断开不会让进程崩溃；
- 监听 socket 是非阻塞的（否则 accept 会阻塞整个事件循环）；
- 每个 fd 都有明确的 owner，删除时不会悬垂。

---

## 第 2 章 HTTP/1.1 解析：字节流不是消息

### 2.1 核心概念

TCP 是字节流，HTTP 是消息。一次 `recv` 可能得到：

- 半个请求头；
- 一个半请求；
- 请求头 + 部分 body；
- 多个 pipeline 请求。

因此解析器必须是**增量状态机**，输入 `Buffer`，返回 `NeedMore/Complete/Error`，而不是“读一次就 parse”。

### 2.2 请求头解析顺序

1. 找 `\r\n\r\n`，限制最大头长度；
2. 解析请求行：方法、target、版本；
3. 逐行解析头：拒绝 obs-fold、非法 token、控制字符；
4. 校验 Host、Content-Length、Transfer-Encoding；
5. 解析 `Connection`、`Expect`、`Upgrade`；
6. 消费整个头，body 留在 Buffer。

实现参考：[`include/edgegate/http.hpp`](include/edgegate/http.hpp)、[`src/http.cpp`](src/http.cpp)。

### 2.3 请求走私类检查

必须拒绝：

- `Content-Length` 与 `Transfer-Encoding` 同时出现；
- 多个冲突的 `Content-Length`；
- 非法 chunk 大小/缺少 CRLF；
- obs-fold；
- 头名/头值中的控制字符。

这些不是“洁癖”，而是代理作为边界时最重要的安全输入校验。写测试时，每一条都应有 400/431 用例。

### 2.4 chunked 解码

状态机：

```text
SizeLine -> Data -> DataCrlf -> SizeLine
   |                              |
   +-- size==0 --> TrailerLine --> Done
```

要点：

- chunk extension 可以丢弃；
- `0\r\n\r\n` 是结束；
- trailer 解析后可以保存，但当前代理不转发；
- 解码器消费输入 Buffer，返回的 view 只在下次 append/reserve 前有效。

### 2.5 响应解析

响应 body 的结束条件取决于：

- 请求方法是不是 HEAD；
- 状态码是不是 1xx/204/304；
- 有没有 `Transfer-Encoding: chunked`；
- 有没有 `Content-Length`；
- 否则读到连接关闭。

这五个分支必须写测试。错误实现通常会在 HEAD/204 上多读一个 body，导致连接池里残留字节，下一条响应错位。

### 2.6 检查点自测

- 一次只喂 1 个字节，解析器能工作吗？
- 请求头 + body 在同一次 recv 中，body 会丢吗？
- chunked 的 `0\r\n\r\n` 后面还有 trailer，能处理吗？
- HEAD 响应带 `Content-Length: 100`，代理会读 100 字节吗？
- 上游响应没有 CL/TE，你怎么判断结束？

### 2.7 面试追问

- 为什么不能用一个 `recv` 直接 `read` 到 `\r\n\r\n`？
- 为什么解析器要消费头部而不是整包？
- `Transfer-Encoding` 和 `Content-Length` 同时出现为什么危险？
- chunked 的 trailer 怎么处理？
- 响应头解析错误时返回 502 还是 400？

---

## 第 3 章 事件循环与连接生命周期

### 3.1 epoll 的三种关注

- 监听 fd：`EPOLLIN`，accept；
- 客户端 fd：读请求时 `EPOLLIN`，有输出时 `EPOLLOUT`；
- 上游 fd：connect 时 `EPOLLOUT`，连接后按需 `EPOLLIN/EPOLLOUT`。

### 3.2 部分读写

`send()` 返回的字节数可能小于请求；`recv()` 可能一次只来一半。正确模式：

```cpp
while (!out.empty()) {
    ssize_t n = send(fd, out.data(), out.size(), MSG_NOSIGNAL);
    if (n > 0) out.consume(n);
    else if (n < 0 && errno == EINTR) continue;
    else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    else { fail(); }
}
```

- 发送成功但未发完 → 保留剩余，关注 `EPOLLOUT`；
- `EAGAIN` → 正常，等下一次可写；
- `EPIPE/ECONNRESET` → 对端断开，进入失败处理；
- 必须用 `MSG_NOSIGNAL` 或全局忽略 SIGPIPE，否则写已关闭连接会杀死进程。

### 3.3 fd 生命周期

经典 bug：处理 A 的事件时关闭 B，而 B 的事件还在同一批 `epoll_wait` 结果里。解决方案：

- 事件循环按 fd 查 handler，删除后查不到就跳过；
- `close_later(fd)` 在本批结束后统一 close，避免 fd 号在批次内被 accept 复用；
- 连接对象标记 `closed/dead`，回调返回后不再访问。

### 3.4 超时

EdgeGate 用 100 ms tick 扫描所有连接，而不是为每个连接维护精确 timer：

- 实现简单，适合教学；
- 10k 连接时每秒 10 万次检查，成本可接受；
- 精度是 100 ms 级，不是 µs 级。

生产实现可以用时间轮/最小堆，但要处理取消、重排和 fd 复用，复杂度更高。

### 3.5 检查点自测

- 客户端只发一半头就停，什么时候超时？
- 上游 connect 返回 `EINPROGRESS`，怎么判断成功/失败？
- 客户端在响应中途断开，上游连接还能回池吗？
- 上游在请求体没发完时返回 413，怎么处理？
- 关闭 fd 后，同一批 epoll 事件还会回调吗？

### 3.6 面试追问

- LT 和 ET 的区别？本项目用哪种？
- 为什么 accept 要循环到 `EAGAIN`？
- `EPOLLRDHUP` 和 `EPOLLHUP` 有什么区别？
- 如何避免 fd 复用导致的“旧响应发给新连接”？
- 单线程 epoll 的吞吐瓶颈在哪里？

---

## 第 4 章 上游连接池、负载均衡与健康检查

### 4.1 连接池

池的本质是“复用已经建立、且仍然干净的 TCP 连接”。复用条件：

- 上游响应是 keep-alive；
- 响应有明确帧边界；
- 连接上没有多余字节；
- 请求没有提前中止；
- 不是协议升级。

复用前用 `recv(MSG_PEEK|MSG_DONTWAIT)` 探活：

- 返回 0 → 对端关闭；
- 返回 >0 → 有脏数据（可能是上一条响应的残留）；
- 返回 -1 且 `EAGAIN` → 干净。

### 4.2 每后端上限

`active + idle` 超过 `max_connections_per_backend` 时：

- 不再新建连接；
- 换一个后端；
- 所有后端都满 → 502。

为什么 idle 也算？因为 idle 连接占用 fd 和内核内存，不能无限囤积。

### 4.3 负载均衡

- round-robin：简单、均匀；不考虑负载差异；
- least-conn：按活跃连接数选，适合长请求/慢后端；平局按轮询顺序，避免饥饿。

### 4.4 健康检查

主动检查：

- 每 `health_interval_ms` 发 `GET health_path`；
- 2xx/3xx = 成功；
- 失败累计到阈值 → unhealthy；
- 成功累计到阈值 → healthy。

被动检查：

- 连接失败 → 立即冷却；
- 读写失败/5xx → 失败计数；
- 成功响应 → 清零。

**关键**：健康检查不是“替代被动失败”，二者要结合。只靠主动检查会在两次检查之间继续打挂掉的后端；只靠被动检查会在没有流量时永远不知道后端恢复。

### 4.5 检查点自测

- 连接池里的连接被后端关了，怎么发现？
- 为什么健康检查要有 healthy/unhealthy 两个阈值？
- drain 和 unhealthy 有什么区别？
- least-conn 的“连接数”应该包含 idle 吗？
- 所有后端都不可用时返回 502 还是 503？

### 4.6 面试追问

- 连接池如何避免惊群/重复建连？
- 健康检查会不会把后端打挂？
- 如何做半开恢复（half-open）？
- 长连接和负载均衡的相互作用？
- 如果后端是 HTTP/2，连接池设计要怎么变？

---

## 第 5 章 背压：本项目最值得深入的部分

### 5.1 先算一笔账

后端 1 GB/s，客户端 10 KB/s，代理每读 64 KiB 就 append：

- 1 秒后：1 GB 缓冲；
- 10 个客户端：10 GB；
- 进程被 OOM。

**必须停止读上游**，而不是“读进来再丢”。

### 5.2 水位实现

```cpp
if (out.size() >= high) {
    enable_upstream_read(false);   // MOD 掉 EPOLLIN
}
```

客户端可写事件：

```cpp
flush_client_out();
if (out.size() < low) {
    enable_upstream_read(true);
}
```

请求方向对称：上游 out 高水位 → 暂停客户端读。

### 5.3 全局预算

单连接有界不代表总内存有界。`max_total_buffered_bytes` 是最后一道保险；超过后所有连接进入全局背压，降到 75% 解除。

### 5.4 慢客户端超时

如果 out 非空且长时间没有写进展，直接关闭。否则一个客户端可以永远占着连接和缓冲。

### 5.5 怎么验证

不要只看“代码里有 if”。用慢客户端 + 指标：

- 响应 4 MiB；
- 客户端 16 KiB / 20 ms；
- 轮询 `/metrics` 的 `edgegate_buffered_bytes` 和 `edgegate_upstream_read_paused_total`；
- 看进程 RSS。

参考：[`docs/backpressure.md`](docs/backpressure.md)、集成测试 `test_backpressure`。

### 5.6 检查点自测

- 高水位是硬上限吗？为什么？
- 暂停上游读后，怎么恢复？
- 客户端 out 为空时还需要 EPOLLOUT 吗？
- 全局预算和单连接水位分别解决什么问题？
- 慢客户端超时和读超时的区别？

### 5.7 面试追问

- 如果内核 socket 缓冲已经吸收了几 MB，应用层水位还有意义吗？
- 如何测量“代理确实在背压”而不是“客户端恰好读得快”？
- 背压会不会导致上游超时？怎么区分正常慢和故障？
- 多路复用（HTTP/2）下背压怎么做？

---

## 第 6 章 重试：不能无条件重发

### 6.1 先问“后端有没有可能已经执行”

| 场景 | 后端可见副作用 | 能否重试 |
| --- | --- | --- |
| connect 失败 | 没有 | 能 |
| 写出请求头前失败 | 没有 | 能 |
| POST 写出 body 后断连 | 可能有 | 不能 |
| GET 写出请求后断连 | 无副作用 | 能 |
| 收到部分响应 | 已执行 | 不能 |

### 6.2 实现要点

- 跟踪 `any_request_bytes_written`（实际 `send` 成功），而不是“append 到 out”；
- body 重放缓冲有上限；
- 重试前重置响应解析器；
- 排除已尝试后端；
- 达到 `max_attempts` 返回 502。

参考：[`include/edgegate/retry.hpp`](include/edgegate/retry.hpp)、[`docs/retry-and-errors.md`](docs/retry-and-errors.md)。

### 6.3 检查点自测

- POST 在 connect 阶段失败，能重试吗？
- PUT body 已发一半，能重试吗？
- GET 收到 1 字节响应后断连，能重试吗？
- 重试时 body 计数会重复吗？
- 为什么不能重试 5xx？

### 6.4 面试追问

- 什么是幂等？PUT 和 POST 的区别？
- 如何用 Idempotency-Key 做安全重试？
- 重试风暴怎么避免？
- 超时和失败的语义区别？
- 代理重试和客户端重试如何分工？

---

## 第 7 章 chunked、keep-alive 与协议转换

### 7.1 四种 body 组合

| 客户端 | 上游 | 处理 |
| --- | --- | --- |
| CL | CL | 流式原样 |
| chunked | chunked | 解码后重新分帧 |
| 无 | 无 | 不发送 CL |
| 上游 close-delimited | 客户端 1.1 | 代理重新 chunked，保持客户端 keep-alive |

### 7.2 为什么重新分帧

上游 close-delimited 以 EOF 结束；如果代理直接转发并保持客户端连接，客户端不知道何时结束。重新 chunked 给客户端明确边界。

### 7.3 100-continue

代理立即回 100，删除 `Expect` 再转发 body。比“等上游 100”简单，但语义上允许服务器提前回 100；生产实现可以做成延迟决策。

### 7.4 管线化

客户端可能在收到响应前发送下一个请求。EdgeGate 在响应完成后才处理缓冲中的下一个请求，避免输出缓冲无限增长；这是“允许管线化但延迟处理”，不是完整流水线。

### 7.5 检查点自测

- 上游 chunked 响应，客户端 HTTP/1.0，怎么处理？
- HEAD 响应有 `Content-Length`，代理会发 body 吗？
- 204 响应能带 `Transfer-Encoding` 吗？
- 客户端 `Connection: close`，上游连接还能回池吗？
- 上游 101 怎么处理？

---

## 第 8 章 可观测性与管理面板

### 8.1 为什么代理需要 trace

出现问题时的第一反应通常是“哪个后端？耗时多少？重试了吗？body 多大？”——如果只能看日志文本，很难快速回答。EdgeGate 给每个请求一个 ID，把：

- request id、method、path、client；
- backend、attempts、retried；
- status、duration、ttfb、connect；
- bytes in/out、chunked、keep-alive；
- error

放进环形 trace，并暴露 `/api/requests/<id>`。

### 8.2 指标设计

- counter：只增，适合 `rate()`；
- gauge：可增可减，适合当前连接/缓冲；
- histogram：固定桶，适合分位数估计。

不要把高基数标签（request id、path）放进 Prometheus 标签；本项目的 path 只在 trace/日志里，不进指标。

### 8.3 管理面板

面板不是“炫技”，而是把后端状态、缓冲、最近请求放在一屏。面试时可以现场打开，解释每个数字来自哪里。

### 8.4 检查点自测

- `requests_total` 和 `responses_total` 为什么会暂时不相等？
- `buffered_bytes` 包含哪些缓冲？
- P95 桶估计和压测器真实 P95 有什么区别？
- 如何用指标判断“上游慢”还是“客户端慢”？

---

## 第 9 章 测试、基准与调试

### 9.1 单元测试

纯逻辑优先：

- Buffer；
- 请求/响应解析；
- chunked；
- 配置；
- 重试规则；
- 负载均衡。

这些不需要起 socket，跑得快，失败定位准。

### 9.2 集成测试

启动真实进程 + 原始 socket，覆盖：

- keep-alive；
- 文件上传/下载哈希；
- chunked；
- 100-continue；
- 故障转移；
- 重试；
- 慢客户端；
- 管理端。

Python 标准库足够，不依赖 curl/ab/wrk。

### 9.3 基准

- 直连 vs 代理；
- 1/8/32 连接；
- 小响应 vs 64 MiB；
- 固定后端、响应大小、TLS（无）等条件；
- 至少 3 轮取中位数；
- 同时记录 P50/P95/P99、错误率、内存。

### 9.4 调试清单

| 现象 | 优先检查 |
| --- | --- |
| 502 | `/api/status` 后端健康、trace error、上游端口 |
| 504 | connect/response timeout、后端是否阻塞 |
| 下载截断 | 响应 body 模式、CL/chunked、上游是否提前关闭 |
| 内存上涨 | `/metrics` buffered_bytes、慢客户端、全局预算 |
| 卡死 | 是否暂停读后未恢复、EPOLLOUT 是否注册 |
| 连接泄漏 | idle 池上限、idle 超时、fd 数量 |
| 哈希不一致 | chunked 重新分帧、body 丢弃、上传目录不一致 |

---

## 第 10 章 TLS 终止：非阻塞握手与 ALPN

> 这一章对应 v1.1 的第一块新增能力。目标不是“支持 HTTPS”四个字，而是能解释清楚非阻塞 TLS 如何与 epoll 配合。

### 10.1 先想清楚：TLS 加在哪一层

EdgeGate 的选择：

- TLS **只作用于客户端侧**：浏览器/curl → EdgeGate 是 TLS；
- EdgeGate → 后端仍是明文 HTTP/1.1；
- 明文 8080 和 TLS 8443 可以同时监听；
- TLS 握手完成前不解析 HTTP；
- ALPN 决定握手后走 h1 还是 h2。

配置：

```ini
listen = 0.0.0.0:8080
tls_listen = 0.0.0.0:8443
tls_cert = certs/cert.pem
tls_key = certs/key.pem
tls_min_version = 1.2
http2 = true
```

生成测试证书：

```bash
bash scripts/gen_test_cert.sh certs
```

### 10.2 非阻塞握手的三个返回值

`SSL_accept` 不是“成功/失败”两个状态：

```text
SSL_accept
  ├─ 1                                  -> 握手完成
  ├─ SSL_ERROR_WANT_READ                -> 等 EPOLLIN
  ├─ SSL_ERROR_WANT_WRITE               -> 等 EPOLLOUT
  └─ 其他                                -> 失败/关闭
```

`TlsConnection` 把它们映射为：

```cpp
enum class IoStatus { Ok, WantRead, WantWrite, Eof, Error };
```

`ClientConn` 的握手状态只关注需要的事件：

```text
WantRead  -> EPOLLIN
WantWrite -> EPOLLOUT
Ok        -> 读 ALPN，创建 h1 或 h2 路径
```

**常见错误**：把 `WANT_WRITE` 当错误关闭；握手会永远完不成。

### 10.3 握手完成前不要解析 HTTP

TLS 记录层不是 HTTP 字节流。`ClientConn` 在 `tls_handshaking=true` 时：

- 只调用 `handle_tls_handshake()`；
- 不进入 `on_readable()` 的 HTTP 解析；
- 握手完成后才 `update_events()` 并开始读请求。

### 10.4 ALPN

服务端列表：

```text
http2=true : \x02h2\x08http/1.1
http2=false: \x08http/1.1
```

使用 `SSL_select_next_proto`；没有交集时 `SSL_TLSEXT_ERR_NOACK`，按 h1 处理。

握手后：

```cpp
SSL_get0_alpn_selected(ssl, &data, &len);
```

- `"h2"` → `ClientConn::start_h2()`；
- 其他/空 → 继续 h1。

### 10.5 超时与关闭

- 握手超时：`tls_handshake_timeout_ms`，100 ms tick 检查；
- 关闭：发一次 `SSL_shutdown`（close_notify），不等待对端，然后延迟关闭 fd；
- 不等待是为了避免慢客户端拖住关闭；严格双向关闭不在 v1.1 范围。

### 10.6 验收

```bash
./build/edgegate --config config/edgegate-tls.conf
curl -k --http1.1 -o /dev/null -w '%{http_version}\n' https://127.0.0.1:8443/api/info
# 1.1
curl -k --http2  -o /dev/null -w '%{http_version}\n' https://127.0.0.1:8443/api/info
# 2
openssl s_client -connect 127.0.0.1:8443 -alpn h2 -brief </dev/null
```

### 10.7 检查点

1. `SSL_write` 返回 `WANT_READ` 时应该关注什么事件？为什么？
2. 握手未完成时收到 EPOLLIN，能直接解析 HTTP 吗？
3. ALPN 没有交集时应该断开还是回落到 h1？
4. 为什么 `close_notify` 只发一次、不等待？
5. 证书/私钥不匹配应该在什么时候发现？

### 10.8 面试追问

- 非阻塞 TLS 与阻塞 TLS 的状态机差异？
- TLS 1.3 的握手为什么比 1.2 少一个 RTT？
- ALPN 和 SNI 分别解决什么问题？
- 会话复用/0-RTT 的风险？
- 为什么上游 TLS 是另一个问题（证书验证、连接池、SNI）？

---

## 第 11 章 HTTP/2 前端：stream、流控与 h2→h1

> HTTP/2 不是“更快的 HTTP/1.1”，而是完全不同的连接复用模型。v1.1 只做前端 h2，后端仍是 h1。

### 11.1 先理解差异

| 维度 | HTTP/1.1 | HTTP/2 |
| --- | --- | --- |
| 并发 | 多连接 / pipelining | 一个连接多 stream |
| 头部 | 文本 | HPACK 二进制 |
| 流量控制 | 无（靠 TCP） | 连接 + stream 窗口 |
| 生命周期 | 连接级 | stream 级 |
| 协议协商 | 无 | ALPN（TLS） |

EdgeGate 的映射：

```text
h2 stream 1 ─┐
h2 stream 3 ─┼─> 每个 H2Stream 一个 HTTP/1.1 上游交换
h2 stream 5 ─┘
```

### 11.2 nghttp2 session

创建：

```cpp
nghttp2_session_callbacks_new(&callbacks);
nghttp2_session_callbacks_set_on_begin_headers_callback(...);
nghttp2_session_callbacks_set_on_header_callback(...);
nghttp2_session_callbacks_set_on_data_chunk_recv_callback(...);
nghttp2_session_callbacks_set_on_frame_recv_callback(...);
nghttp2_session_callbacks_set_on_stream_close_callback(...);
nghttp2_session_server_new2(&session, callbacks, this, nullptr);
```

提交设置：

```cpp
nghttp2_settings_entry settings[] = {
  {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 32},
  {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 1 << 20},
};
nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, settings, 2);
```

I/O：

- `nghttp2_session_mem_recv` 吃 TLS 读到的字节；
- `nghttp2_session_mem_send` 产生待发帧，写入 `H2Session::out`，再 `SSL_write`；
- `mem_send` 必须在回调外调用；回调里只改状态，调用 `schedule_flush()` 延迟发送。

### 11.3 stream 生命周期

```text
HEADERS -> on_begin_headers
           on_header(:method/:path/:authority/...)
           on_frame_recv(END_STREAM?)
                -> on_end_stream -> dispatch
DATA    -> on_data_chunk_recv -> 追加 body
           on_frame_recv(END_STREAM)
                -> on_end_stream -> dispatch
RST/完成 -> on_stream_close -> 中止上游、提交 trace、延迟回收
```

`nghttp2_session_set_stream_user_data(session, id, stream)` 让回调能找回 `H2Stream*`。

**不要在 on_stream_close 里直接 delete**：nghttp2 回调栈还可能引用；放入 `erase_queue`，在 `mem_recv`/`mem_send` 返回后 `reap()`。

### 11.4 请求体为什么先缓冲

h2 请求体没有天然长度。要转成 h1，有两条路：

1. 等 END_STREAM，用 Content-Length；
2. 流式转成 h1 chunked，做 h2 窗口 ↔ 上游写缓冲联动。

v1.1 选 1：

- 请求头/响应头映射简单；
- 请求体可重放，重试逻辑与 h1 一致；
- 不需要把 h2 流控窗口更新与上游背压做双向联动。

代价：

- 单流 `http2_max_request_body_bytes`（默认 8 MiB）；
- 单连接 `http2_max_total_request_body_bytes`（默认 32 MiB）；
- 超限返回 413。

**这是明确取舍，不是“已经支持流式 h2 上传”。**

### 11.5 响应：data provider 与背压

h2 响应不能一次性生成后 `submit` 完事。使用 data provider：

```cpp
provider.source.ptr = stream;
provider.read_callback = &H2Stream::data_read;
nghttp2_submit_response2(session, id, nva, nvlen, &provider);
```

回调：

- `pending` 有数据 → 拷贝，返回字节数；
- `pending` 为空且未完成 → `NGHTTP2_ERR_DEFERRED`；
- 上游数据到达 → `nghttp2_session_resume_data`；
- 响应完成且 pending 为空 → `NGHTTP2_DATA_FLAG_EOF`。

背压：

```text
upstream recv -> pending -> data provider -> H2Session out -> TLS
pending >= client_buffer_high -> 暂停上游 EPOLLIN
provider 消费到 low -> 恢复
```

集成测试用 `curl --http2 --limit-rate 256k` 下载 1 MiB，确认 `upstream_read_paused_total > 0` 且 `buffered_bytes < 2 MiB`。

### 11.6 重试与错误

- h2 请求体已完整缓冲，满足“可重放”；
- 仍然遵守 h1 规则：非幂等请求写出字节后不重试；已收到响应不重试；
- 连接失败/响应前断开 → GET/HEAD 可换后端重试；
- 已提交响应头后上游失败 → `RST_STREAM(INTERNAL_ERROR)`；
- 未提交响应头 → 502/504 响应；
- 超时：`upstream_response_timeout_ms` 按 stream 计时。

### 11.7 多路复用怎么验证

不要只看“用了 nghttp2”。用：

```bash
curl -k --http2 --parallel \
  -o /tmp/a -o /tmp/b \
  -w '%{url_effective} %{http_version} %{time_total}\n' \
  https://127.0.0.1:8443/api/delay?ms=400 \
  https://127.0.0.1:8443/api/info
```

同时检查管理端：

```bash
curl -s http://127.0.0.1:9900/metrics | grep client_connections_total
```

两个请求都 `http_version=2`，总墙钟约 0.4–0.5 s，且 `client_connections_total` 只增加 1，说明共用一个 TLS 连接、两个 stream。

### 11.8 检查点

1. `on_stream_close` 为什么不能直接 delete stream？
2. h2 请求体为什么缓冲？上限是多少？超限返回什么？
3. data provider 返回 `NGHTTP2_ERR_DEFERRED` 后，什么时候恢复？
4. h2 的多路复用与 h1 keep-alive 有什么本质区别？
5. h2 已提交响应头后上游断开，为什么不能发 502？
6. 如何证明两个请求真的在同一个连接上？

### 11.9 面试追问

- HPACK 解决什么问题？动态表的风险？
- h2 流控窗口与 TCP 窗口的关系？
- 为什么 h2 需要 GOAWAY？本项目做了吗？
- h2 请求体流式转发会引入哪些状态？
- 单线程 h2 session 的瓶颈在哪里？
- 如何做 h2 上游/端到端 h2？

---

## 第 12 章 简历写法与面试准备

### 12.1 简历条目（只写已实现且能解释的）

> **EdgeGate｜C++20 HTTP/1.1 + HTTP/2 反向代理与负载均衡器**
>
> 基于 epoll 实现单线程非阻塞代理：HTTP/1.1 支持 keep-alive、CL/chunked 双向流式转发、100-continue、HEAD/204/304 与 HTTP/1.0；实现每后端连接池、round-robin/least-conn、主动/被动健康检查与故障恢复；设计双向缓冲水位和全局预算，慢客户端场景下应用层缓冲稳定在 256 KiB、RSS 约 8 MiB；按方法、已写字节和 body 可重放性实现安全重试，POST 写出后不重发；v1.1 增加 OpenSSL 非阻塞 TLS 终止、ALPN 与 nghttp2 HTTP/2 前端（h2 stream → HTTP/1.1 上游），支持多路复用、流级响应背压与 h2 重试；提供 Prometheus 指标、请求级 trace 和管理面板；Python 端到端测试 26 个场景覆盖 TLS/h2、上传下载哈希、故障转移、重试和背压。

**写法纪律**：

- 数字来自你自己的机器和 `docs/benchmark.md`；TLS/HTTP2 没有吞吐基准数字，不要编；
- 不要说“支持百万并发”“生产级”；
- TLS/HTTP2 只写你真正跑通并讲得清的部分：ALPN、非阻塞握手、h2 stream、流控、背压；
- 每个名词都要能展开 2–3 分钟。

### 12.2 高频追问

**网络/协议**

- 为什么用 epoll？LT/ET 区别？
- 非阻塞 connect 怎么判断成功？
- 部分写怎么处理？
- chunked 怎么解码/重新分帧？
- 请求走私怎么防？
- HTTP/1.0 和 1.1 的 keep-alive 差异？
- 100-continue 的意义？
- 非阻塞 TLS 的 WANT_READ/WANT_WRITE 怎么映射到 epoll？
- ALPN 的作用？没有交集怎么办？
- HTTP/2 的 stream、HPACK、流控窗口分别是什么？
- h2 stream 如何映射到 h1 上游？

**代理/可靠性**

- 连接池复用条件？
- 健康检查阈值怎么定？
- drain 和 unhealthy 的区别？
- 重试为什么不能无条件？
- 超时/失败/5xx 的语义区别？
- 慢客户端怎么防 OOM？
- 全局预算和单连接水位的关系？
- h2 请求体为什么缓冲？上限多少？超限怎么办？
- h2 响应为什么用 data provider，而不是一次性生成？
- h2 已提交响应头后上游断开怎么处理？

**性能**

- 单线程瓶颈在哪？
- 8/32 连接吞吐为什么远低于直连？
- 怎么测背压是否真的生效？
- 如何减少每请求分配/系统调用？
- 多 worker 会带来哪些一致性问题？

**通用**

- 最难调的 bug 是什么？（准备 1–2 个真实故事，例如“上游 EPOLLOUT 未处理导致请求永远不发出”“chunked 终止帧重复导致下一条响应错位”“h2 stream 在回调里被 delete 导致悬垂”）
- 如果重做，哪里会不同？（例如“响应超时应从实际 flush 完成开始计时”“access log 应异步”“h2 请求体应流式而不是缓冲”）

### 12.3 现场演示

1. 启动 3 个后端 + 代理；
2. 浏览器打开管理面板；
3. `curl` 上传/下载并 `sha256sum`；
4. 杀掉一个后端，展示健康状态变化（h1 与 h2 各一次）；
5. 运行慢客户端脚本，展示 `buffered_bytes` 和暂停次数；
6. `curl -k --http2 --parallel` 展示两个 stream 共用一个连接；
7. `curl -k --http2 --limit-rate 256k` 展示 h2 响应背压；
8. 跑 `eg_loadgen`，解释直连 vs 代理差异。

---

## 附录 A 常用命令

```bash
# 构建 + 测试
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure

# 只跑单元测试
ctest --test-dir build -E integration_test

# 启动演示
mkdir -p /tmp/egdemo/root /tmp/egdemo/uploads
./build/eg_demo --listen 127.0.0.1:9001 --name app-a --root /tmp/egdemo/root --uploads /tmp/egdemo/uploads &
./build/eg_demo --listen 127.0.0.1:9002 --name app-b --root /tmp/egdemo/root --uploads /tmp/egdemo/uploads &
./build/eg_demo --listen 127.0.0.1:9003 --name app-c --root /tmp/egdemo/root --uploads /tmp/egdemo/uploads &
./build/edgegate --config config/edgegate.conf

# 检查配置
./build/edgegate --config config/edgegate.conf --check-config

# 压测
./build/eg_loadgen --url http://127.0.0.1:8080 --path /api/info \
  --connections 8 --requests 12500 --warmup 1000

# 慢客户端
python3 scripts/slow_client.py --port 8080 --admin 9900 --mb 8

# 清理
pkill -x edgegate; pkill -x eg_demo
```

## 附录 B 5–7 周计划

| 周 | 内容 | 完成标志 |
| --- | --- | --- |
| W1 | Buffer、HTTP 解析、chunked、配置 | 单元测试全绿；curl 能拿到 200 |
| W2 | epoll、客户端/上游状态机、部分读写 | keep-alive、真实 GET/HEAD/204 通过 |
| W3 | 连接池、LB、健康检查、超时 | 杀后端能摘除/恢复；无连接泄漏 |
| W4 | 背压、重试、chunked 转换 | 慢客户端缓冲有界；POST 不重试 |
| W5 | 指标、trace、管理面板、集成测试 | 26 个场景通过（h1） |
| W6 | TLS 终止、ALPN、证书、握手超时 | `curl --http1.1` 与 `--http2` 都能协商 |
| W7 | HTTP/2 session/stream、多路复用、h2 背压 | h2 GET/POST/上传/下载/413/504/重试通过 |

## 附录 C 配置速查

见 [`config/edgegate.conf`](config/edgegate.conf) 与 README 的配置表。优先理解：

- `max_attempts` / `retry_idempotent_only`；
- 四个水位 + `max_total_buffered_bytes`；
- `max_connections_per_backend` / `max_idle_connections_per_backend`；
- 三类超时：客户端、上游、总请求；
- 健康检查间隔/阈值/冷却；
- TLS：`tls_listen`、`tls_cert`、`tls_key`、`tls_min_version`、`tls_handshake_timeout_ms`；
- HTTP/2：`http2`、`http2_max_request_body_bytes`、`http2_max_total_request_body_bytes`、`http2_max_concurrent_streams`。

---

> 最后：这个项目最容易“看起来做完了”的地方是 happy path；真正拉开差距的是慢客户端、半截读写、连接复用和重试边界。TLS/HTTP2 又各引入一套状态机：TLS 的 `WANT_READ/WANT_WRITE`，HTTP/2 的 stream 生命周期与流控。每写一个功能，先问“它在什么情况下会坏”，再写测试。
