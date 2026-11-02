# EdgeGate 从零到 GitHub：WSL2 分阶段复刻手册

> 这份文档是**操作手册**，不是架构讲解。目标：从一个空的 WSL 目录开始，按阶段把 EdgeGate 的实现搬进来、构建、测试、提交，最后推送到 GitHub。
>
> 当前参考源码是 **v1.1**（HTTP/1.1 + TLS 终止 + HTTP/2 前端），分阶段路线是 **10 个 commit + `v1.1.0` 标签**。HTTP/2 在最后一阶段打开，前面阶段即使系统没有 nghttp2 也能构建和测试。
>
> 适合三种用法：
>
> - **A. 复刻（推荐先做）**：按阶段从参考源码复制文件，每阶段构建 + 测试 + commit。重点放在读懂模块、验证行为、形成可讲的项目历史。
> - **B. 手写**：把每个阶段的 `cp` 换成自己写，卡住时再看参考实现；构建、测试、commit 的节奏不变。对应章节见 [TUTORIAL.md](../TUTORIAL.md)。
> - **C. 已经按旧版做到 v1.0**：不要从头重来，直接看 [附录 D](#附录-d已经完成-v10增量升级到-v11)。
>
> 预计时间：A 路径 3–5 小时；B 路径按 TUTORIAL 的 5–7 周节奏。
>
> 约定：以下命令**全部在 WSL Ubuntu 终端**执行，不是在 PowerShell。Windows 上的源码位于 `/mnt/d/project/EdgeGate`。

---

## 0. 开始前：环境、Git 与参考源码

### 0.1 进入 WSL

在 PowerShell：

```powershell
wsl -d Ubuntu
```

或者在 Windows Terminal 打开 Ubuntu 标签页。进入后确认：

```bash
pwd
cat /etc/os-release | head -n 3
```

### 0.2 检查工具

```bash
cmake --version | head -1
c++ --version | head -1
ninja --version
python3 --version
git --version
rsync --version | head -1
curl --version | head -1
curl -V | grep -o 'HTTP2' | head -1
openssl version
sha256sum --version | head -1
```

本机预期（2026-10-31 核对）：Ubuntu 26.04、GCC 15.2、CMake 4.2、Ninja、Python 3.14、Git 已安装。

如果有缺失：

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build rsync git python3 curl
```

TLS 与 HTTP/2 的开发依赖（v1.1 需要 OpenSSL 开发包；HTTP/2 需要 nghttp2）：

```bash
sudo apt install -y libssl-dev libnghttp2-dev
```

确认头文件和库都能被编译器找到：

```bash
ls /usr/include/openssl/ssl.h
ls /usr/include/nghttp2/nghttp2.h
```

> 没有 `libnghttp2-dev` 也能构建：CMake 会给出 “building without HTTP/2” 警告，TLS 和 HTTP/1.1 仍然可用，集成测试会跳过 h2 场景。真正启用 HTTP/2 在阶段 9（第 10 节）。
>
> `curl -V` 的输出里如果有 `HTTP2`，集成测试才会实际跑 h2 场景；Ubuntu 自带的 curl 通常支持。

### 0.3 Git 身份

本机已经配置过（`yjwyyds1` / QQ 邮箱）。先确认：

```bash
git config --global user.name
git config --global user.email
git config --global init.defaultBranch
git config --global core.autocrlf
```

如果 `init.defaultBranch` 为空，设置 `main`；把换行策略设为 `input`，避免 Windows 编辑器把脚本改成 CRLF：

```bash
git config --global init.defaultBranch main
git config --global core.autocrlf input
```

如果姓名/邮箱为空：

```bash
git config --global user.name "你的名字"
git config --global user.email "你的邮箱"
```

> **隐私提示**：GitHub 公开仓库会显示 commit 作者邮箱。如果不想公开 QQ 邮箱，到 GitHub `Settings → Emails` 勾选 *Keep my email addresses private*，复制页面给出的 `...@users.noreply.github.com` 地址，再用它设置 `user.email`。是否这样做由你决定。

### 0.4 SSH 与 GitHub 连通性

本机已有 `~/.ssh/id_ed25519`，并且已经能通过 GitHub 认证：

```bash
ls -l ~/.ssh/id_ed25519.pub
ssh -T git@github.com
```

预期：

```text
Hi yjwyyds1! You've successfully authenticated, but GitHub does not provide shell access.
```

`ssh -T` 返回 1 是正常的（GitHub 不提供 shell）；只要看到 `successfully authenticated` 即可。

如果以后换了机器、没有 key：

```bash
ssh-keygen -t ed25519 -C "你的邮箱"
cat ~/.ssh/id_ed25519.pub
```

把输出的公钥复制到 GitHub `Settings → SSH and GPG keys → New SSH key`，再执行 `ssh -T git@github.com`。

### 0.5 参考源码

```bash
REF=/mnt/d/project/EdgeGate
ls "$REF/CMakeLists.txt" "$REF/include/edgegate/buffer.hpp" "$REF/apps/edgegate.cpp"
```

三个文件都存在即可。若 `/mnt/d` 不存在，先在 PowerShell 执行 `wsl -d Ubuntu` 后检查 `ls /mnt`，或在 WSL 里挂载 D 盘。

> `REF` 是当前 shell 的变量；换终端或重新登录后需要重新执行 `REF=/mnt/d/project/EdgeGate`。

> **源码版本**：`$REF` 对应 v1.1。前 8 个阶段先建立 HTTP/1.1 + TLS（HTTP/2 代码以空实现桩参与编译），第 9 阶段再打开 nghttp2。**不要**把 `$REF` 里的 `src/proxy.cpp` 单独复制到旧仓库而不复制 `src/tls.cpp`、`src/h2_impl.inc` 和新的 `CMakeLists.txt`，否则会报找不到头文件或符号。

> **文件权限**：从 `/mnt/d` 复制的文件可能带可执行位（Git 里显示 `100755`）。每个阶段 `git add` 之前建议执行：
>
> ```bash
> find include src tests apps -type f \( -name '*.hpp' -o -name '*.cpp' \) -exec chmod 644 {} + 2>/dev/null || true
> chmod 644 config/*.conf 2>/dev/null || true
> ```
>
> 脚本保持可执行（第 7 阶段 `chmod +x scripts/*.sh scripts/*.py`）。注意：`git add --chmod=-x -A` 在部分 Git 版本对 `-A` 不生效，先在磁盘上 `chmod 644` 再 `git add -A` 最可靠。如果已经提交了 755 的源码文件，可用 `git update-index --chmod=-x <文件>` 修正。

### 0.6 创建干净工作区

```bash
mkdir -p ~/dev
cd ~/dev
mkdir -p EdgeGate
cd EdgeGate
pwd
```

如果你之前已经在这个目录里有内容，先备份或换一个目录名（例如 `EdgeGate-lab`），不要直接覆盖。

> **可选：先做一次环境自检**
>
> 在正式分阶段之前，可以复制一份完整源码到临时目录，确认环境能跑通：
>
> ```bash
> rm -rf ~/dev/EdgeGate-check
> rsync -a --exclude='build*' "$REF/" ~/dev/EdgeGate-check/
> cd ~/dev/EdgeGate-check
> cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
> cmake --build build -j
> ctest --test-dir build --output-on-failure
> ```
>
> 预期 `7/7 tests passed`。确认后可以删除 `EdgeGate-check`，回到 `~/dev/EdgeGate` 按下面步骤正式复刻。这样能把“环境问题”和“分阶段实现问题”分开。

---

## 1. 阶段 0：仓库骨架（commit 1）

### 1.1 初始化 Git

```bash
cd ~/dev/EdgeGate
git init
git status
```

预期看到 `On branch main`、`No commits yet`。

### 1.2 创建 `.gitignore`

```bash
cat > .gitignore <<'EOF'
build/
build-*/
build-*/*
*.o
*.a
*.exe
compile_commands.json
.cache/
EOF
```

### 1.3 创建 `.gitattributes`

```bash
cat > .gitattributes <<'EOF'
* text=auto eol=lf
*.sh text eol=lf
*.py text eol=lf
*.md text eol=lf
EOF
```

### 1.4 创建 README 占位

```bash
cat > README.md <<'EOF'
# EdgeGate

C++20 HTTP/1.1 reverse proxy and load balancer. Work in progress.
EOF
```

### 1.5 第一次提交

```bash
git add .gitignore .gitattributes README.md
git status --short
git commit -m "初始化 EdgeGate 仓库骨架"
git log --oneline
```

### 验收

```bash
git status
```

应显示 `working tree clean`。这一步只证明 Git 可用，不涉及编译。

---

## 2. 阶段 1：Buffer、util、log、event loop（commit 2）

### 先读

- [ARCHITECTURE.md](../ARCHITECTURE.md) 第 3 节（事件循环）和第 4 节（缓冲）
- [TUTORIAL.md](../TUTORIAL.md) 第 1–3 章

### 2.1 复制文件

```bash
cd ~/dev/EdgeGate
mkdir -p include/edgegate src tests

cp "$REF/include/edgegate/buffer.hpp" include/edgegate/
cp "$REF/include/edgegate/util.hpp" include/edgegate/
cp "$REF/src/util.cpp" src/
cp "$REF/include/edgegate/log.hpp" include/edgegate/
cp "$REF/src/log.cpp" src/
cp "$REF/include/edgegate/event_loop.hpp" include/edgegate/

cp "$REF/tests/test_util.hpp" tests/
cp "$REF/tests/buffer_test.cpp" tests/

# 从 /mnt/d 复制过来的文件可能带可执行位；源码统一为 644
find include src tests -type f \( -name '*.hpp' -o -name '*.cpp' \) -exec chmod 644 {} +
```

后面每个阶段复制完也执行一次同样的 `find ... chmod 644`（脚本除外）。

### 2.2 写入阶段 1 的 CMakeLists

```bash
cat > CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.22)
project(EdgeGate VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
    message(FATAL_ERROR "EdgeGate uses Linux epoll; build it inside WSL2 or a Linux host.")
endif()

find_package(Threads REQUIRED)

add_library(edgegate_core
    src/util.cpp
    src/log.cpp
)
target_include_directories(edgegate_core PUBLIC include)
target_compile_options(edgegate_core PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(edgegate_core PUBLIC Threads::Threads)

include(CTest)
if(BUILD_TESTING)
    add_executable(buffer_test tests/buffer_test.cpp)
    target_link_libraries(buffer_test PRIVATE edgegate_core)
    target_compile_options(buffer_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME buffer_test COMMAND buffer_test)
endif()
EOF
```

### 2.3 构建 + 测试

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

预期：

```text
1/1 Test #1: buffer_test ...................... Passed
100% tests passed, 0 tests failed out of 1
```

### 2.4 理解检查

用 2 分钟回答：

1. `Buffer::consume()` 为什么只移动读指针，不立刻搬移内存？
2. `EventLoop::close_later()` 解决什么问题？
3. `reserve()` 在什么情况下压缩、什么情况下扩容？

答不上来就回去看 [ARCHITECTURE.md](../ARCHITECTURE.md) 第 3–4 节。

### 2.5 提交

```bash
find include src tests apps -type f \( -name '*.hpp' -o -name '*.cpp' \) -exec chmod 644 {} + 2>/dev/null || true
chmod 644 config/*.conf 2>/dev/null || true
git add -A
git status --short
git commit -m "添加 Buffer、工具、日志和事件循环"
```

> **想现在就备份到 GitHub？** 可以按第 11.1–11.2 创建空仓库并首次 `git push`；之后每个阶段结束都执行 `git push`。如果你已经推过（例如当前状态），直接继续阶段 2。

---

## 3. 阶段 2：HTTP 解析（commit 3）

### 先读

- [ARCHITECTURE.md](../ARCHITECTURE.md) 第 6 节
- [docs/http-semantics.md](http-semantics.md)
- [TUTORIAL.md](../TUTORIAL.md) 第 2 章

### 3.1 复制文件

```bash
cp "$REF/include/edgegate/http.hpp" include/edgegate/
cp "$REF/src/http.cpp" src/
cp "$REF/tests/http_test.cpp" tests/
```

### 3.2 修改 CMakeLists

把 `add_library(edgegate_core ...)` 改成：

```cmake
add_library(edgegate_core
    src/util.cpp
    src/log.cpp
    src/http.cpp
)
```

在 `if(BUILD_TESTING)` 中、`endif()` 之前追加：

```cmake
    add_executable(http_test tests/http_test.cpp)
    target_link_libraries(http_test PRIVATE edgegate_core)
    target_compile_options(http_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME http_test COMMAND http_test)
```

可以用 `nano CMakeLists.txt` 编辑，或用你习惯的编辑器。

### 3.3 构建 + 测试

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

预期 `2/2 tests passed`。

### 3.4 理解检查

1. 输入只喂 1 个字节时，解析器为什么不能直接报错？
2. `Content-Length` 和 `Transfer-Encoding: chunked` 同时出现为什么要拒绝？
3. HEAD 响应带 `Content-Length: 100` 时，代理应该读 body 吗？

### 3.5 提交

```bash
find include src tests apps -type f \( -name '*.hpp' -o -name '*.cpp' \) -exec chmod 644 {} + 2>/dev/null || true
chmod 644 config/*.conf 2>/dev/null || true
git add -A
git commit -m "实现 HTTP/1.1 解析、chunked 和序列化"
git push
```

---

## 4. 阶段 3：严格配置（commit 4）

### 先读

- [README.md](../README.md) 的“配置”一节
- [TUTORIAL.md](../TUTORIAL.md) 第 4 章

### 4.1 复制文件

```bash
cp "$REF/include/edgegate/config.hpp" include/edgegate/
cp "$REF/src/config.cpp" src/
cp "$REF/tests/config_test.cpp" tests/
```

### 4.2 修改 CMakeLists

在 `add_library(edgegate_core ...)` 中追加：

```cmake
    src/config.cpp
```

在 `if(BUILD_TESTING)` 中追加：

```cmake
    add_executable(config_test tests/config_test.cpp)
    target_link_libraries(config_test PRIVATE edgegate_core)
    target_compile_options(config_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME config_test COMMAND config_test)
```

### 4.3 构建 + 测试

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
```

预期 `3/3 tests passed`。

### 4.4 提交

```bash
find include src tests apps -type f \( -name '*.hpp' -o -name '*.cpp' \) -exec chmod 644 {} + 2>/dev/null || true
chmod 644 config/*.conf 2>/dev/null || true
git add -A
git commit -m "添加严格 INI 配置加载"
git push
```

---

## 5. 阶段 4：指标、后端健康、重试规则（commit 5）

### 先读

- [docs/retry-and-errors.md](retry-and-errors.md)
- [docs/observability.md](observability.md)
- [TUTORIAL.md](../TUTORIAL.md) 第 4、6 章

### 5.1 复制文件

```bash
cp "$REF/include/edgegate/metrics.hpp" include/edgegate/
cp "$REF/src/metrics.cpp" src/
cp "$REF/include/edgegate/backend.hpp" include/edgegate/
cp "$REF/src/backend.cpp" src/
cp "$REF/include/edgegate/retry.hpp" include/edgegate/
cp "$REF/src/retry.cpp" src/

cp "$REF/tests/metrics_test.cpp" tests/
cp "$REF/tests/balancer_test.cpp" tests/
cp "$REF/tests/retry_test.cpp" tests/
```

### 5.2 修改 CMakeLists

`add_library` 中追加：

```cmake
    src/metrics.cpp
    src/backend.cpp
    src/retry.cpp
```

`if(BUILD_TESTING)` 中追加：

```cmake
    add_executable(metrics_test tests/metrics_test.cpp)
    target_link_libraries(metrics_test PRIVATE edgegate_core)
    target_compile_options(metrics_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME metrics_test COMMAND metrics_test)

    add_executable(balancer_test tests/balancer_test.cpp)
    target_link_libraries(balancer_test PRIVATE edgegate_core)
    target_compile_options(balancer_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME balancer_test COMMAND balancer_test)

    add_executable(retry_test tests/retry_test.cpp)
    target_link_libraries(retry_test PRIVATE edgegate_core)
    target_compile_options(retry_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME retry_test COMMAND retry_test)
```

### 5.3 构建 + 测试

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
```

预期 `6/6 tests passed`。

### 5.4 理解检查

1. POST 在 connect 阶段失败能不能重试？为什么？
2. PUT 已经发了一半 body，能不能重试？
3. 健康检查的 healthy/unhealthy 阈值为什么要分开？

### 5.5 提交

```bash
find include src tests apps -type f \( -name '*.hpp' -o -name '*.cpp' \) -exec chmod 644 {} + 2>/dev/null || true
chmod 644 config/*.conf 2>/dev/null || true
git add -A
git commit -m "添加指标、后端健康检查和重试策略"
git push
```

---

## 6. 阶段 5：epoll 代理、TLS 终止、管理端、主程序（commit 6）

### 先读

- [ARCHITECTURE.md](../ARCHITECTURE.md) 第 5、7、8、9 节，以及 6.4（TLS）
- [docs/backpressure.md](backpressure.md)
- [docs/tls.md](tls.md)
- [TUTORIAL.md](../TUTORIAL.md) 第 3、5、6、10 章

### 6.1 复制文件

```bash
cp "$REF/include/edgegate/proxy.hpp" include/edgegate/
cp "$REF/src/proxy.cpp" src/
cp "$REF/include/edgegate/tls.hpp" include/edgegate/
cp "$REF/src/tls.cpp" src/
cp "$REF/src/h2_impl.inc" src/
cp "$REF/include/edgegate/admin.hpp" include/edgegate/
cp "$REF/src/admin.cpp" src/
mkdir -p apps config
cp "$REF/apps/edgegate.cpp" apps/
cp "$REF/config/edgegate.conf" config/
cp "$REF/config/edgegate-tls.conf" config/
```

`h2_impl.inc` 在阶段 5 先作为**空实现桩**参与编译（没有定义 `EDGEGATE_HAVE_NGHTTP2` 时只提供 `H2Session` 的空方法）；阶段 9 打开 nghttp2 后才是完整实现。不要因为“现在用不到”而漏掉它，`src/proxy.cpp` 会直接包含这个文件。

`tls.hpp/.cpp` 是 `proxy.cpp` 的硬依赖，必须一起复制。

### 6.2 修改 CMakeLists

把第一行的工程版本从 `1.0.0` 改成 `1.1.0`（与 `edgegate --version` 的输出一致）：

```cmake
project(EdgeGate VERSION 1.1.0 LANGUAGES CXX)
```

在 `find_package(Threads REQUIRED)` 之后追加：

```cmake
find_package(OpenSSL REQUIRED)
```

`add_library` 中追加：

```cmake
    src/tls.cpp
    src/admin.cpp
    src/proxy.cpp
```

把 `target_link_libraries(edgegate_core ...)` 改成：

```cmake
target_link_libraries(edgegate_core PUBLIC Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
```

在 `if(BUILD_TESTING)` 之前（或之后，只要不在测试块内部）追加：

```cmake
add_executable(edgegate apps/edgegate.cpp)
target_link_libraries(edgegate PRIVATE edgegate_core)
target_compile_options(edgegate PRIVATE -Wall -Wextra -Wpedantic)
```

此时**不要**加入 nghttp2 的查找逻辑；阶段 9 会一次性加进去。现在 `proxy.cpp` 走 `#ifndef EDGEGATE_HAVE_NGHTTP2` 的桩分支，HTTP/2 尚未启用。

### 6.3 构建 + 检查

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/edgegate --version
./build/edgegate --config config/edgegate.conf --check-config
ctest --test-dir build --output-on-failure
```

预期：

```text
edgegate 1.1.0
config OK: listen=0.0.0.0:8080 admin=127.0.0.1:9900 ...
6/6 tests passed
```

此时还没有演示后端，所以不要急着 `curl` 代理；下一阶段补上。

> TLS 端口现在就能启动：生成测试证书后可以用 `openssl s_client` 或 `curl -k` 验证 HTTP/1.1 over TLS。HTTP/2 要等阶段 9。
>
> ```bash
> bash "$REF/scripts/gen_test_cert.sh" certs
> # 需要先启动至少一个后端；没有后端时 TLS 握手仍能成功，请求会得到 503
> ```

### 6.4 提交

```bash
find include src tests apps -type f \( -name '*.hpp' -o -name '*.cpp' -o -name '*.inc' \) -exec chmod 644 {} + 2>/dev/null || true
chmod 644 config/*.conf 2>/dev/null || true
git add -A
git commit -m "实现 epoll 代理、TLS 终止和管理端"
git push
```

---

## 7. 阶段 6：演示后端与压测器（commit 7）

### 先读

- [README.md](../README.md) 的“演示后端与压测器”一节
- [TUTORIAL.md](../TUTORIAL.md) 第 9 章

### 7.1 复制文件

```bash
cp "$REF/apps/demo_backend.cpp" apps/
cp "$REF/apps/eg_loadgen.cpp" apps/
```

### 7.2 修改 CMakeLists

在 `edgegate` 可执行目标之后追加：

```cmake
add_executable(eg_demo apps/demo_backend.cpp)
target_link_libraries(eg_demo PRIVATE edgegate_core)

add_executable(eg_loadgen apps/eg_loadgen.cpp)
target_link_libraries(eg_loadgen PRIVATE edgegate_core)
```

演示后端用 OpenSSL 的 SHA-256。阶段 5 已经把 `OpenSSL::Crypto` 链接进 `edgegate_core`（`PUBLIC`），所以 `eg_demo` 不需要再单独 `find_library(crypto)`。

### 7.3 构建 + 冒烟

```bash
cmake --build build -j
./build/eg_demo --help | head -20
./build/eg_loadgen --help | head -20
```

预期能看到参数列表。

> `ninja: no work to do` 只是表示没有新改动，属于正常输出。只有 CMake 明确报 `OpenSSL not found` 时才需要重装 `libssl-dev`：
>
> ```bash
> sudo apt install -y libssl-dev
> rm -rf build
> cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
> cmake --build build -j
> ```

### 7.4 提交

```bash
find include src tests apps -type f \( -name '*.hpp' -o -name '*.cpp' -o -name '*.inc' \) -exec chmod 644 {} + 2>/dev/null || true
chmod 644 config/*.conf 2>/dev/null || true
git add -A
git commit -m "添加文件/API 演示后端和压测器"
git push
```

---

## 8. 阶段 7：端到端测试与脚本（commit 8）

### 8.1 复制文件

```bash
cp "$REF/tests/integration_test.py" tests/
mkdir -p scripts
cp "$REF/scripts/smoke.sh" scripts/
cp "$REF/scripts/bench.sh" scripts/
cp "$REF/scripts/quick_bench.sh" scripts/
cp "$REF/scripts/slow_client.py" scripts/
cp "$REF/scripts/gen_test_cert.sh" scripts/
cp "$REF/scripts/tls_smoke.sh" scripts/
cp "$REF/scripts/h2_manual.sh" scripts/
chmod +x scripts/*.sh scripts/*.py
```

同时把测试证书排除在 Git 之外（`.gitignore` 追加）：

```bash
cat >> .gitignore <<'EOF'
certs/
*.pem
EOF
```

> 证书是本地测试生成的，**不要**提交私钥。集成测试会在临时目录里自己生成证书。

### 8.2 修改 CMakeLists

在 `endif()`（测试块结束）之前追加：

```cmake
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME integration_test
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration_test.py
                         --binary-dir ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(integration_test PROPERTIES TIMEOUT 300)
        set_tests_properties(integration_test PROPERTIES ENVIRONMENT "no_proxy=*;NO_PROXY=*")
    endif()
```

`ENVIRONMENT` 那一行是为了避免 WSL 里从 Windows 继承的 `http_proxy` 把 `127.0.0.1` 也代理出去。

### 8.3 全量测试

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

预期：

```text
1/7 buffer_test ...................... Passed
2/7 http_test ........................ Passed
3/7 config_test ...................... Passed
4/7 balancer_test .................... Passed
5/7 retry_test ....................... Passed
6/7 metrics_test ..................... Passed
7/7 integration_test ................. Passed
100% tests passed, 0 tests failed out of 7
```

本阶段还没有启用 nghttp2，所以集成测试输出应是：

```text
integration_test: 19/19 passed, workspace=/tmp/...
```

其中包含 TLS HTTP/1.1；h2 场景因为 ALPN 协商不到 `h2` 而跳过。阶段 9 打开 HTTP/2 后会变成 **26/26**。（如果 `curl` 不支持 TLS 或缺少 `openssl`，TLS 场景也会跳过，显示 `18/18`；按 0.2 节装好依赖即可。）

若失败，先清理端口占用：

```bash
pkill -x edgegate 2>/dev/null || true
pkill -x eg_demo 2>/dev/null || true
ss -ltnp | grep -E ':(9001|9002|9003|8080|9900)\b' || true
```

### 8.4 手工跑一遍真实服务

> 如果你的 `~/.bashrc` 导出了 `http_proxy/https_proxy/all_proxy`（WSL 常见），`curl` 可能把 `127.0.0.1` 也发给 Windows 代理，报 `Failed to connect ... via 10.255.255.254`。当前 `scripts/smoke.sh` 已在脚本开头设置 `no_proxy=*`；如果你用的是旧副本，先手动 export：

```bash
export no_proxy='*'
export NO_PROXY='*'
bash scripts/smoke.sh
```

这个脚本会：

- 建 `/tmp/eg-smoke/root`、`/tmp/eg-smoke/uploads`；
- 启动两个 `eg_demo`（9101/9102）和一个 `edgegate`（9200/9201）；
- 验证 `/api/info`、文件下载 SHA-256、1 MiB 上传、chunked echo、管理端指标。

预期最后能看到：

```text
=== download ===
hello edgegate
<同一个 sha256>  /tmp/eg-smoke/root/blob.bin
<同一个 sha256>  /tmp/eg-smoke/blob.out
=== upload ===
{"instance":"app-a",...,"sha256":"..."}
<同一个 sha256>  /tmp/eg-smoke/upload.bin
<同一个 sha256>  /tmp/eg-smoke/up.out
=== chunked request ===
{"instance":"app-a","bytes":13,"sha256":"8d9fb419..."}
```

TLS 单独冒烟（脚本自己生成证书、启动一个 demo 后端和一个带 TLS 的代理）：

```bash
bash scripts/tls_smoke.sh
```

脚本会依次打印：TLS HTTP/1.1 响应头、文件下载、HTTP/2 GET、`http_version=2`、h2 POST echo、2 MiB 下载、ALPN/cipher。**阶段 7 时 h2 还没启用**，所以 `http_version=` 会是 `1.1`，这是正常的；阶段 9 再跑一次才是 `2`。

> 如果 `curl` 报 `Failed to connect ... via 10.255.255.254`，先 `export no_proxy='*'`；脚本内部已经设置。

### 8.5 提交

```bash
git add -A
git commit -m "添加端到端集成测试和脚本"
git push
```

---

## 9. 阶段 8：文档（commit 9）

### 9.1 复制文档

```bash
cp "$REF/README.md" .
cp "$REF/ARCHITECTURE.md" .
cp "$REF/TUTORIAL.md" .
mkdir -p docs
cp "$REF/docs/"* docs/
```

这会用完整 README 覆盖第 1 阶段的占位 README。

### 9.2 检查

```bash
git status --short
ls docs
head -n 5 README.md
```

### 9.3 提交

```bash
git add -A
git commit -m "添加架构、教程和基准文档"
git push
```

---

## 10. 阶段 9：启用 HTTP/2、Release 构建与 v1.1.0 标签（commit 10）

前 8 个阶段已经有 TLS 终止和完整的 h2 代码，但 `EDGEGATE_HAVE_NGHTTP2` 没有定义，`h2_impl.inc` 走的是空实现桩。这一阶段把 nghttp2 接进来，并在真实流量上验证 ALPN `h2`、多路复用和流级背压。

### 10.1 安装 nghttp2 并修改 CMakeLists

```bash
sudo apt install -y libnghttp2-dev
ls /usr/include/nghttp2/nghttp2.h
```

在 `find_package(OpenSSL REQUIRED)` 之后追加：

```cmake
# HTTP/2 前端使用 nghttp2；如果系统没有开发包，TLS/HTTP1.1 仍然可以构建，
# 只是无法协商 h2。WSL 安装：sudo apt install libnghttp2-dev
find_path(NGHTTP2_INCLUDE_DIR NAMES nghttp2/nghttp2.h)
find_library(NGHTTP2_LIBRARY NAMES nghttp2)
if(NOT NGHTTP2_LIBRARY)
    find_file(NGHTTP2_LIBRARY NAMES libnghttp2.so.14
              PATHS /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu /usr/lib64 /lib64)
endif()

set(EDGEGATE_HAVE_NGHTTP2 OFF)
if(NGHTTP2_INCLUDE_DIR AND NGHTTP2_LIBRARY)
    set(EDGEGATE_HAVE_NGHTTP2 ON)
    message(STATUS "EdgeGate: HTTP/2 enabled (nghttp2 ${NGHTTP2_LIBRARY})")
else()
    message(WARNING "EdgeGate: nghttp2 not found; building without HTTP/2 (TLS/1.1 still available)")
endif()
```

在 `target_link_libraries(edgegate_core PUBLIC Threads::Threads OpenSSL::SSL OpenSSL::Crypto)` 之后追加：

```cmake
if(EDGEGATE_HAVE_NGHTTP2)
    target_include_directories(edgegate_core PRIVATE ${NGHTTP2_INCLUDE_DIR})
    target_link_libraries(edgegate_core PUBLIC ${NGHTTP2_LIBRARY})
    target_compile_definitions(edgegate_core PUBLIC EDGEGATE_HAVE_NGHTTP2=1)
endif()
```

`src/h2_impl.inc` 在阶段 5 已经复制过，不用再动。

### 10.2 重新配置、构建、全量测试

CMake 缓存里没有新变量，必须重新执行一次 `cmake -S . -B build`：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

配置阶段应看到：

```text
-- EdgeGate: HTTP/2 enabled (nghttp2 /usr/lib/x86_64-linux-gnu/libnghttp2.so)
```

如果仍然看到 `nghttp2 not found`，确认 `libnghttp2-dev` 已安装，然后删除缓存重来：

```bash
rm -rf build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
```

全量测试：

```bash
ctest --test-dir build --output-on-failure
```

预期 `7/7`，集成测试输出：

```text
integration_test: 26/26 passed, workspace=/tmp/...
```

比阶段 7 多出的 7 个场景是：HTTP/2 basic + POST、多路复用、chunked/HEAD/204/upload、请求体 413、上游超时 504、慢客户端背压、重试语义。

### 10.3 手工验证 h2

```bash
# 证书
bash scripts/gen_test_cert.sh certs

# 启动演示后端与启用 TLS 的代理
mkdir -p /tmp/egdemo/root /tmp/egdemo/uploads
echo "hello h2" > /tmp/egdemo/root/hello.txt
./build/eg_demo --listen 127.0.0.1:9001 --name app-a \
  --root /tmp/egdemo/root --uploads /tmp/egdemo/uploads &
./build/edgegate --config config/edgegate-tls.conf &
```

另开一个终端：

```bash
export no_proxy='*'
curl -k --http2 -o /dev/null -w 'HTTP version: %{http_version}\n' \
  https://127.0.0.1:8443/api/info

# 多路复用：两个请求共用一个 TLS 连接，总耗时约等于较慢的那个
curl -k --http2 --parallel -o /tmp/a -o /tmp/b -w '%{http_version} %{time_total}\n' \
  'https://127.0.0.1:8443/api/delay?ms=400' \
  'https://127.0.0.1:8443/api/info'

curl -s http://127.0.0.1:9900/metrics | grep client_connections_total
```

预期：`HTTP version: 2`；两个请求都是 `2`，总墙钟约 0.4–0.5 s；`client_connections_total` 只增加 1。

也可以直接跑：

```bash
bash scripts/tls_smoke.sh
bash scripts/h2_manual.sh
```

`h2_manual.sh` 覆盖并行流、chunked/close-delimited 响应、HEAD/204、2 MiB echo、PUT 上传、4 MiB 慢下载和 `upstream_read_paused_total`。

结束手工进程：

```bash
pkill -x edgegate 2>/dev/null || true
pkill -x eg_demo 2>/dev/null || true
```

### 10.4 提交 HTTP/2

```bash
find include src tests apps -type f \( -name '*.hpp' -o -name '*.cpp' -o -name '*.inc' \) -exec chmod 644 {} + 2>/dev/null || true
chmod 644 config/*.conf 2>/dev/null || true
git add -A
git commit -m "添加 HTTP/2 前端（nghttp2）"
git push
```

### 10.5 Release 构建

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
ctest --test-dir build-release --output-on-failure
```

预期 `7/7` 通过，集成测试 `26/26`。Release 构建同样应打印 `HTTP/2 enabled`。

打标签：

```bash
git tag -a v1.1.0 -m "EdgeGate v1.1.0：TLS 终止与 HTTP/2 前端"
git show --stat v1.1.0 | head -30
```

查看完整历史：

```bash
git log --oneline --reverse
```

应该看到 10 个提交，从 `初始化 EdgeGate 仓库骨架` 到 `添加 HTTP/2 前端（nghttp2）`（文档 commit 在阶段 8，位置按实际顺序）。

> 如果你是从旧版 v1.0 升级，提交数量可能不同；以 `git log` 为准，标签仍然打在最后一个功能 commit 上。

---

## 11. GitHub：首次关联、每阶段推送与发布验证

> 推荐在完成阶段 1 后就创建空仓库并首次推送；之后每个阶段 `git commit` 后执行 `git push`。这样 GitHub 上始终有可恢复的进度。
>
> **如果你已经完成首次推送**（例如当前状态：`origin/main` 已经有阶段 0/1 两条 commit），跳过 11.1–11.2，直接从阶段 2 继续，每阶段结束 `git push` 即可。

### 11.1 创建空仓库

在浏览器打开 <https://github.com/new>：

- Owner：`yjwyyds1`（或你的账号）
- Repository name：`EdgeGate`
- Description：`C++20 HTTP/1.1 + HTTP/2 reverse proxy with epoll, TLS, connection pooling and backpressure`
- 选择 **Private** 或 **Public**（建议先 Private，确认没有隐私内容后再改 Public）
- **不要**勾选 Add a README / .gitignore / license

创建后复制 SSH 地址，形如：

```text
git@github.com:yjwyyds1/EdgeGate.git
```

> 本机 SSH 已认证为 `yjwyyds1`；如果你的 GitHub 用户名不同，把下面的 `GH_USER` 改成自己的。

> 这台机器没有安装 `gh`，但 SSH 已经认证；用网页建空仓库最省事。以后想用 `gh` 再单独安装，不是必需。

### 11.2 首次关联并推送 `main`

```bash
cd ~/dev/EdgeGate
GH_USER=yjwyyds1
REPO=EdgeGate

git remote -v
git remote add origin "git@github.com:${GH_USER}/${REPO}.git"
git branch -M main

git push -u origin main
```

如果提示 `remote origin already exists`：

```bash
git remote set-url origin "git@github.com:${GH_USER}/${REPO}.git"
```

如果提示 `src refspec main does not match any`：说明还没有 commit，回到第 1 阶段检查 `git log`。

> 首次推送只推 `main`，**不要**在这里推 tag；`v1.1.0` 要等阶段 9 所有 commit 完成后再打、再推。

### 11.3 后续每个阶段推送

完成第 2–9 阶段时，每次 commit 后执行：

```bash
cd ~/dev/EdgeGate
git push
```

如果提示 `fatal: The current branch main has no upstream branch`：

```bash
git push -u origin main
```

### 11.4 发布 `v1.1.0` 标签

阶段 9 的 Release 构建和 `26/26` 集成测试通过后：

> 如果第 10.5 节已经创建了 `v1.1.0`，跳过 `git tag -a`，直接执行两条 push；重复创建会提示 `tag already exists`。

```bash
cd ~/dev/EdgeGate
git tag -a v1.1.0 -m "EdgeGate v1.1.0：TLS 终止与 HTTP/2 前端"
git show --stat v1.1.0 | head -30

git push origin main
git push origin v1.1.0
```

如果 tag 已存在但指错了 commit：

```bash
git tag -d v1.1.0
git tag -a v1.1.0 -m "EdgeGate v1.1.0：TLS 终止与 HTTP/2 前端"
git push origin :refs/tags/v1.1.0
git push origin v1.1.0
```

### 11.5 验证

```bash
git remote -v
git log --oneline --decorate --graph --all | head -30
git status
git ls-remote --heads origin
git ls-remote --tags origin
```

浏览器打开 `https://github.com/yjwyyds1/EdgeGate`，在**全部阶段完成后**确认：

- 10 个 commit（从仓库骨架到 HTTP/2 前端）；
- `v1.1.0` tag；
- README 正常渲染（Mermaid 图在 GitHub 上会显示）；
- 没有 `build/`、`build-release/`、`.o`、`certs/` 文件。

> 如果你现在只完成到阶段 1，看到 2 个 commit 是正常的；每完成一个阶段并 push，commit 数会逐步增加。

### 11.6 从 GitHub 重新克隆验证

这是最容易被忽略但最重要的一步：

```bash
GH_USER=yjwyyds1
REPO=EdgeGate

cd /tmp
rm -rf EdgeGate-verify
git clone "git@github.com:${GH_USER}/${REPO}.git" EdgeGate-verify
cd EdgeGate-verify
git log --oneline | head
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

如果全新 clone 也能 `7/7` 通过（集成测试 `26/26`），说明：没有漏提交文件、没有依赖本地 build 缓存、换行和脚本权限都正确。全新机器先安装 `libssl-dev libnghttp2-dev`；CMake 输出应包含 `HTTP/2 enabled`，否则 h2 场景会被跳过，测试数会少 7 个。

### 11.7 可选：仓库信息

在 GitHub 仓库页面：

- 右侧 About → 编辑 Description、Website、Topics：
  - `cpp20`, `http-proxy`, `http2`, `tls`, `epoll`, `load-balancer`, `backpressure`, `systems-programming`
- 如果先建了 Private，确认内容可公开后再 `Settings → General → Change visibility → Public`。
- 如果要让别人复用代码，添加 LICENSE（MIT/Apache-2.0 等）；不添加则默认保留所有权利。

---

## 12. 常见问题

| 现象 | 处理 |
| --- | --- |
| `cmake: command not found` | `sudo apt install -y cmake ninja-build build-essential` |
| `ninja: command not found` | 同上，或 `cmake -G "Unix Makefiles"` |
| `libcrypto not found` / `Could NOT find OpenSSL` | `sudo apt install -y libssl-dev`，然后删掉 `build/` 重新配置 |
| CMake 警告 `nghttp2 not found` | 只影响 HTTP/2；`sudo apt install -y libnghttp2-dev` 后 `rm -rf build` 重新配置 |
| 集成测试只有 19 个而不是 26 个 | 没有 nghttp2，ALPN 协商不到 h2；安装 `libnghttp2-dev` 并重新配置 |
| `curl -V` 里没有 `HTTP2` | curl 不支持 h2；安装 `libcurl4-openssl-dev` 或换用系统 curl；也可以只用 `openssl s_client -alpn h2` 观察 ALPN |
| TLS 启动报 `load certificate` / `private key` | 先 `bash scripts/gen_test_cert.sh certs`；确认证书与私钥匹配、路径相对启动目录 |
| `curl` 报 `certificate verify failed` | 自签名证书只用于测试，加 `-k`；生产环境应使用受信任证书 |
| `http_version` 输出 `1.1` | 客户端没协商到 h2：检查 `http2 = true`、CMake 是否 `HTTP/2 enabled`、`curl --http2`、是否走的 8443 端口 |
| `No tests were found` | 确认 `-DBUILD_TESTING=ON`、`ctest --test-dir build -N` |
| 集成测试端口冲突 | `pkill -x edgegate; pkill -x eg_demo`；`ss -ltnp \| grep -E ':(9001\|9002\|9003\|8080\|9900)'` |
| 脚本 `Permission denied` | `chmod +x scripts/*.sh scripts/*.py` |
| 脚本出现 `$'\r': command not found` | 文件被改成 CRLF：`git config --global core.autocrlf input`，`sed -i 's/\r$//' scripts/*.sh` |
| `fatal: not a git repository` | `cd ~/dev/EdgeGate` |
| `src refspec main does not match any` | 还没有 commit；先 `git log` 确认 |
| `remote origin already exists` | `git remote set-url origin ...` |
| `Permission denied (publickey)` | `ssh -T git@github.com`；检查 key 是否加到 GitHub |
| `Support for password authentication was removed` | 用 SSH remote 或 Personal Access Token，不要用账号密码 |
| `curl` 报 `Failed to connect ... via 10.255.255.254` | WSL 的 `.bashrc` 导出了 http_proxy；本地请求执行 `export no_proxy='*'; export NO_PROXY='*'`，或使用已修复的 `scripts/smoke.sh` |
| GitHub 连接超时 | 先 `curl -I https://github.com`；如公司/校园网限制，配置代理或 SSH over 443 |
| `dubious ownership` | 仓库放在 `/mnt/d` 时常见；本手册用 `~/dev`，一般不会遇到。必要时 `git config --global --add safe.directory <path>` |
| 误把 build 提交了 | `git rm -r --cached build build-release`，确认 `.gitignore` 后重新 commit |

### SSH over 443（仅在 22 端口被限制时）

编辑 `~/.ssh/config`：

```text
Host github.com
    HostName ssh.github.com
    Port 443
    User git
```

然后 `ssh -T git@github.com`。本机当前直连 22 端口正常，不需要这一步。

---

## 13. 提交历史与每步证明了什么

| # | Commit | 证明 |
| --- | --- | --- |
| 1 | `初始化 EdgeGate 仓库骨架` | 仓库、忽略规则、换行策略 |
| 2 | `添加 Buffer、工具、日志和事件循环` | 缓冲/工具/日志/事件循环可独立单测 |
| 3 | `实现 HTTP/1.1 解析、chunked 和序列化` | 增量 HTTP 解析、chunked、错误输入 |
| 4 | `添加严格 INI 配置加载` | 配置严格校验 |
| 5 | `添加指标、后端健康检查和重试策略` | 指标、健康状态、重试纯逻辑 |
| 6 | `实现 epoll 代理、TLS 终止和管理端` | epoll 代理、TLS 握手/ALPN、连接池、背压、管理端 |
| 7 | `添加文件/API 演示后端和压测器` | 真实文件/API 服务与压测器 |
| 8 | `添加端到端集成测试和脚本` | 19 个端到端场景（含 TLS h1） |
| 9 | `添加架构、教程和基准文档` | 架构、教程、设计与基准 |
| 10 | `添加 HTTP/2 前端（nghttp2）` | h2 多路复用、流级背压、26 个端到端场景 |
| tag | `v1.1.0` | 可复现的发布点 |

**面试时可以按这个顺序讲**：先讲协议解析和事件循环，再讲连接池/健康/重试，然后讲 TLS 与 HTTP/2 的传输/流模型，最后讲背压和可观测性。每个 commit 都是一个可验证的里程碑。

---

## 14. 最终检查清单

```bash
cd ~/dev/EdgeGate

# 1. 工作区干净
git status

# 2. 历史与标签
git log --oneline --decorate --graph --all
git tag -n

# 3. 本地 Release 全量测试
ctest --test-dir build-release --output-on-failure

# 4. 冒烟
bash scripts/smoke.sh
bash scripts/tls_smoke.sh
bash scripts/h2_manual.sh

# 5. HTTP/2 构建确认
cmake -S . -B build -G Ninja 2>&1 | grep 'HTTP/2 enabled'

# 6. 远端
git remote -v
git ls-remote --heads origin
git ls-remote --tags origin
```

全部通过后，你就可以：

- 用 `git clone` 在另一台机器复现；
- 在 GitHub 上展示 commit 历史和 tag；
- 按 [TUTORIAL.md](../TUTORIAL.md) 第 12 章的简历/面试提纲准备讲解；
- 继续做 h2c、流式 h2 请求体、多 worker 或上游 TLS 等扩展。

---

## 附录 A：文件清单（按阶段）

| 阶段 | 新增文件 |
| --- | --- |
| 0 | `.gitignore`, `.gitattributes`, `README.md` |
| 1 | `include/edgegate/{buffer,util,log,event_loop}.hpp`, `src/{util,log}.cpp`, `tests/{test_util.hpp,buffer_test.cpp}`, `CMakeLists.txt` |
| 2 | `include/edgegate/http.hpp`, `src/http.cpp`, `tests/http_test.cpp` |
| 3 | `include/edgegate/config.hpp`, `src/config.cpp`, `tests/config_test.cpp` |
| 4 | `include/edgegate/{metrics,backend,retry}.hpp`, `src/{metrics,backend,retry}.cpp`, 3 个测试 |
| 5 | `include/edgegate/{proxy,admin,tls}.hpp`, `src/{proxy,admin,tls}.cpp`, `src/h2_impl.inc`, `apps/edgegate.cpp`, `config/{edgegate,edgegate-tls}.conf` |
| 6 | `apps/{demo_backend,eg_loadgen}.cpp` |
| 7 | `tests/integration_test.py`, `scripts/{smoke,bench,quick_bench,gen_test_cert,tls_smoke,h2_manual}.sh`, `scripts/slow_client.py` |
| 8 | `README.md`（完整版）、`ARCHITECTURE.md`、`TUTORIAL.md`、`docs/*` |
| 9 | `CMakeLists.txt`（nghttp2 发现与链接；无新增源文件） |

## 附录 B：最短命令串（熟练后）

```bash
# 环境自检
cmake --version && c++ --version && git --version
sudo apt install -y libssl-dev libnghttp2-dev

# 建仓库
mkdir -p ~/dev/EdgeGate && cd ~/dev/EdgeGate && git init

# 之后按第 1–9 阶段执行；每阶段：
cmake --build build -j && ctest --test-dir build --output-on-failure
find include src tests apps -type f \( -name '*.hpp' -o -name '*.cpp' -o -name '*.inc' \) -exec chmod 644 {} + 2>/dev/null || true
chmod 644 config/*.conf 2>/dev/null || true
git add -A && git commit -m "..."
git push

# 发布（阶段 9 启用 nghttp2 后）
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j && ctest --test-dir build-release --output-on-failure
git tag -a v1.1.0 -m "EdgeGate v1.1.0：TLS 终止与 HTTP/2 前端"

# 推送（首次在阶段 1 后执行 add remote / push -u；之后每阶段 git push）
git remote add origin git@github.com:yjwyyds1/EdgeGate.git
git branch -M main
git push -u origin main
git push origin v1.1.0
```

---

## 附录 C：已经提交了英文 commit，怎么改中文

如果像下面这样已经提交了两条英文 message：

```text
e422079 feat(core): add buffer, utilities, logging and event loop
0d81825 chore: initialize EdgeGate repository skeleton
```

在**还没有 remote**或**还没推送**时，直接交互式 rebase：

```bash
cd ~/dev/EdgeGate
git config --global core.editor nano
git rebase -i --root
```

把两行 `pick` 改成 `reword`，保存退出，然后依次输入：

```text
初始化 EdgeGate 仓库骨架
添加 Buffer、工具、日志和事件循环
```

再次执行时，哈希会变化，这是正常的。

如果源码文件被记录成 `100755`：

```bash
find include src tests apps -type f \( -name '*.hpp' -o -name '*.cpp' -o -name '*.inc' \) -exec chmod 644 {} +
chmod 644 config/*.conf 2>/dev/null || true
git add -A
git commit --amend --no-edit
git ls-files -s | grep 100755 || echo "no executable source files"
```

如果**已经推送过**，改写历史后需要：

```bash
git push --force-with-lease origin main
```

如果 tag 已经指向旧 commit：

```bash
git tag -d v1.1.0
git tag -a v1.1.0 -m "EdgeGate v1.1.0：TLS 终止与 HTTP/2 前端"
git push origin :refs/tags/v1.1.0
git push origin v1.1.0
```

---

## 附录 D：已经完成 v1.0，增量升级到 v1.1

适用于这种情况：仓库里已经有 v1.0 的 6–9 个 commit（例如本机：阶段 0–6 已推送，阶段 7 的文件还在工作区），现在想加上 TLS 与 HTTP/2，而**不想重做整个历史**。

升级后的历史大致是：

```text
... 之前的 v1.0 commit ...
添加 TLS 终止和 HTTP/2 前端        <- 本次
添加架构、教程和基准文档（v1.1）    <- 如果阶段 8 还没做
tag: v1.1.0
```

不要改写已经推送的 v1.0 commit。它们如实记录了当时的实现；v1.1 用新 commit 叠加即可。

### D.1 先把阶段 7 收尾

如果 `git status` 里还有阶段 7 的 `tests/integration_test.py`、`scripts/`、`CMakeLists.txt` 改动，先用 v1.0 的测试提交它们（这一步的集成测试是 18 个场景）：

```bash
cd ~/dev/EdgeGate
git status --short

cmake --build build -j
ctest --test-dir build --output-on-failure        # 预期 7/7，integration 18/18

find include src tests apps -type f \( -name '*.hpp' -o -name '*.cpp' \) -exec chmod 644 {} + 2>/dev/null || true
chmod 644 config/*.conf 2>/dev/null || true
git add -A
git commit -m "添加端到端集成测试和脚本"
git push
```

如果阶段 7 已经提交，跳过这一步。

> 你的 commit 6 信息是 `实现 epoll 代理、管理端和安全重试`，与本手册新读者的 `实现 epoll 代理、TLS 终止和管理端` 不同，这是正常的：你的那条 commit 确实还不含 TLS。不需要为了对齐文字去改写已推送的历史。

### D.2 安装 TLS/HTTP2 依赖

```bash
sudo apt update
sudo apt install -y libssl-dev libnghttp2-dev
ls /usr/include/openssl/ssl.h /usr/include/nghttp2/nghttp2.h
```

### D.3 覆盖为 v1.1 文件

```bash
REF=/mnt/d/project/EdgeGate
cd ~/dev/EdgeGate

# 构建与核心
cp "$REF/CMakeLists.txt" .
cp "$REF/include/edgegate/config.hpp" include/edgegate/
cp "$REF/include/edgegate/proxy.hpp" include/edgegate/
cp "$REF/include/edgegate/tls.hpp" include/edgegate/
cp "$REF/src/config.cpp" src/
cp "$REF/src/proxy.cpp" src/
cp "$REF/src/tls.cpp" src/
cp "$REF/src/h2_impl.inc" src/
cp "$REF/apps/edgegate.cpp" apps/
cp "$REF/config/edgegate-tls.conf" config/

# 测试
cp "$REF/tests/config_test.cpp" tests/
cp "$REF/tests/integration_test.py" tests/

# 脚本（tls/h2 冒烟、证书生成；同时同步 smoke 的 no_proxy 修复）
cp "$REF/scripts/"*.sh scripts/
cp "$REF/scripts/slow_client.py" scripts/
chmod +x scripts/*.sh scripts/*.py

# 不提交证书
cat >> .gitignore <<'EOF'
certs/
*.pem
EOF

# 从 /mnt/d 复制过来的源码统一 644
find include src tests apps -type f \( -name '*.hpp' -o -name '*.cpp' -o -name '*.inc' \) -exec chmod 644 {} + 2>/dev/null || true
chmod 644 config/*.conf 2>/dev/null || true
```

核对关键文件都在：

```bash
ls src/tls.cpp src/h2_impl.inc include/edgegate/tls.hpp config/edgegate-tls.conf
git status --short
```

`git status` 里应该只有上述文件被修改/新增，**不应该**出现 `certs/` 或 `*.pem`。

### D.4 重新配置、构建、全量测试

CMakeLists 变了，必须重新配置（不能只 `cmake --build`）：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

配置输出应包含：

```text
-- EdgeGate: HTTP/2 enabled (nghttp2 /usr/lib/x86_64-linux-gnu/libnghttp2.so)
```

测试：

```bash
ctest --test-dir build --output-on-failure
./build/edgegate --version
```

预期：

```text
edgegate 1.1.0
...
integration_test: 26/26 passed, workspace=/tmp/...
100% tests passed, 0 tests failed out of 7
```

如果还是 19/19，说明 nghttp2 没被找到：确认 `libnghttp2-dev` 已安装，然后 `rm -rf build` 重新配置。

### D.5 手工验证 TLS 与 HTTP/2

```bash
export no_proxy='*'
export NO_PROXY='*'

bash scripts/gen_test_cert.sh certs

# 终端 A：启动一个后端和 TLS 代理
mkdir -p /tmp/egdemo/root /tmp/egdemo/uploads
echo "hello v1.1" > /tmp/egdemo/root/hello.txt
./build/eg_demo --listen 127.0.0.1:9001 --name app-a \
  --root /tmp/egdemo/root --uploads /tmp/egdemo/uploads &
./build/edgegate --config config/edgegate-tls.conf &
```

另开终端或直接跑脚本：

```bash
bash scripts/tls_smoke.sh
bash scripts/h2_manual.sh
```

单独确认 ALPN 与多路复用：

```bash
curl -k --http2 -o /dev/null -w 'HTTP version: %{http_version}\n' \
  https://127.0.0.1:8443/api/info
# HTTP version: 2

curl -k --http2 --parallel -o /tmp/a -o /tmp/b \
  -w '%{http_version} %{time_total}\n' \
  'https://127.0.0.1:8443/api/delay?ms=400' \
  'https://127.0.0.1:8443/api/info'
```

结束后清理：

```bash
pkill -x edgegate 2>/dev/null || true
pkill -x eg_demo 2>/dev/null || true
```

### D.6 提交 v1.1 代码

```bash
git add -A
git status --short
git commit -m "添加 TLS 终止和 HTTP/2 前端"
git push
```

### D.7 补上阶段 8 文档（如果还没做）

```bash
cp "$REF/README.md" .
cp "$REF/ARCHITECTURE.md" .
cp "$REF/TUTORIAL.md" .
cp "$REF/docs/"* docs/

git add -A
git commit -m "添加架构、教程和基准文档（v1.1）"
git push
```

### D.8 Release 构建、标签、远端验证

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
ctest --test-dir build-release --output-on-failure        # 7/7，integration 26/26

git tag -a v1.1.0 -m "EdgeGate v1.1.0：TLS 终止与 HTTP/2 前端"
git push origin main
git push origin v1.1.0
```

全新 clone 再验一次：

```bash
cd /tmp
rm -rf EdgeGate-verify
git clone git@github.com:yjwyyds1/EdgeGate.git EdgeGate-verify
cd EdgeGate-verify
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

应看到 `HTTP/2 enabled`、`7/7` 和 `26/26`。

### D.9 常见偏差

| 现象 | 原因 / 处理 |
| --- | --- |
| `src/tls.cpp: No such file` | 只复制了 `proxy.cpp`，漏了 TLS 文件；按 D.3 完整复制 |
| `h2_impl.inc: No such file` | 漏了 `src/h2_impl.inc`；它被 `proxy.cpp` 直接包含 |
| `undefined reference to SSL_*` | CMakeLists 不是 v1.1 版本，`edgegate_core` 没链接 `OpenSSL::SSL` |
| `http_version` 仍是 `1.1` | 没装 `libnghttp2-dev` 或没重新配置 CMake；看配置输出有没有 `HTTP/2 enabled` |
| 集成测试 19/19 | 没有 h2；同上 |
| `git status` 里出现 `certs/` | `.gitignore` 没追加成功；`git rm -r --cached certs` 后确认忽略规则 |
| 源码显示 100755 | 重新执行 D.3 末尾的 `find ... chmod 644`，再 `git add -A` |

---

> 这份手册的目标不是“把文件复制过去”，而是让你在每一步都能回答：**我加了什么、为什么这样分层、测试证明了什么、下一个 commit 为什么存在。** 如果某一步想深入，回到 TUTORIAL 对应章节，而不是继续机械复制。
