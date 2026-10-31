# EdgeGate 从零到 GitHub：WSL2 分阶段复刻手册

> 这份文档是**操作手册**，不是架构讲解。目标：从一个空的 WSL 目录开始，按阶段把 EdgeGate 的实现搬进来、构建、测试、提交，最后推送到 GitHub。
>
> 适合两种用法：
>
> - **A. 复刻（推荐先做）**：按阶段从参考源码复制文件，每阶段构建 + 测试 + commit。重点放在读懂模块、验证行为、形成可讲的项目历史。
> - **B. 手写**：把每个阶段的 `cp` 换成自己写，卡住时再看参考实现；构建、测试、commit 的节奏不变。对应章节见 [TUTORIAL.md](../TUTORIAL.md)。
>
> 预计时间：A 路径 2–4 小时；B 路径按 TUTORIAL 的 4–6 周节奏。
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
sha256sum --version | head -1
```

本机预期（2026-10-31 核对）：Ubuntu 26.04、GCC 15.2、CMake 4.2、Ninja、Python 3.14、Git 已安装。

如果有缺失：

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build rsync git python3 curl
```

> 演示后端只依赖 `libcrypto` 运行库。若第 6 阶段 CMake 报 `libcrypto not found`，再安装：
>
> ```bash
> sudo apt install -y libssl-dev
> ```

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

## 6. 阶段 5：epoll 代理、管理端、主程序（commit 6）

### 先读

- [ARCHITECTURE.md](../ARCHITECTURE.md) 第 5、7、8、9 节
- [docs/backpressure.md](backpressure.md)
- [TUTORIAL.md](../TUTORIAL.md) 第 3、5、6 章

### 6.1 复制文件

```bash
cp "$REF/include/edgegate/proxy.hpp" include/edgegate/
cp "$REF/src/proxy.cpp" src/
cp "$REF/include/edgegate/admin.hpp" include/edgegate/
cp "$REF/src/admin.cpp" src/
mkdir -p apps config
cp "$REF/apps/edgegate.cpp" apps/
cp "$REF/config/edgegate.conf" config/
```

### 6.2 修改 CMakeLists

`add_library` 中追加：

```cmake
    src/admin.cpp
    src/proxy.cpp
```

在 `if(BUILD_TESTING)` 之前（或之后，只要不在测试块内部）追加：

```cmake
add_executable(edgegate apps/edgegate.cpp)
target_link_libraries(edgegate PRIVATE edgegate_core)
target_compile_options(edgegate PRIVATE -Wall -Wextra -Wpedantic)
```

### 6.3 构建 + 检查

```bash
cmake --build build -j
./build/edgegate --version
./build/edgegate --config config/edgegate.conf --check-config
ctest --test-dir build --output-on-failure
```

预期：

```text
edgegate 1.0.0
config OK: listen=0.0.0.0:8080 admin=127.0.0.1:9900 ...
6/6 tests passed
```

此时还没有演示后端，所以不要急着 `curl` 代理；下一阶段补上。

### 6.4 提交

```bash
find include src tests apps -type f \( -name '*.hpp' -o -name '*.cpp' \) -exec chmod 644 {} + 2>/dev/null || true
chmod 644 config/*.conf 2>/dev/null || true
git add -A
git commit -m "实现 epoll 代理、管理端和安全重试"
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

在 `find_package(Threads REQUIRED)` 之后加入：

```cmake
# 演示后端用 OpenSSL 的 SHA-256；有些最小镜像只有运行库（libcrypto.so.3），
# 没有开发头文件，所以同时接受 -dev 和运行时 soname。
find_library(CRYPTO_LIBRARY NAMES crypto)
if(NOT CRYPTO_LIBRARY)
    find_file(CRYPTO_LIBRARY NAMES libcrypto.so.3 libcrypto.so.1.1
              PATHS /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu /usr/lib64 /lib64)
endif()
if(NOT CRYPTO_LIBRARY)
    message(FATAL_ERROR "libcrypto not found. Install libssl-dev (or libssl3) to build the demo backend.")
endif()
```

在 `edgegate` 可执行目标之后追加：

```cmake
add_executable(eg_demo apps/demo_backend.cpp)
target_link_libraries(eg_demo PRIVATE edgegate_core ${CRYPTO_LIBRARY})

add_executable(eg_loadgen apps/eg_loadgen.cpp)
target_link_libraries(eg_loadgen PRIVATE edgegate_core)
```

### 7.3 构建 + 冒烟

```bash
cmake --build build -j
./build/eg_demo --help | head -20
./build/eg_loadgen --help | head -20
```

预期能看到参数列表。

> 如果 `eg_demo --help` 已经正常输出，说明 libcrypto 已经找到，**不要**执行下面的重装/重建；`ninja: no work to do` 只是表示没有新改动，属于正常输出。

若 CMake 在第一次配置时没找到 libcrypto：

```bash
sudo apt install -y libssl-dev
rm -rf build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

### 7.4 提交

```bash
find include src tests apps -type f \( -name '*.hpp' -o -name '*.cpp' \) -exec chmod 644 {} + 2>/dev/null || true
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
chmod +x scripts/*.sh scripts/*.py
```

### 8.2 修改 CMakeLists

在 `endif()`（测试块结束）之前追加：

```cmake
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME integration_test
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration_test.py
                         --binary-dir ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(integration_test PROPERTIES TIMEOUT 300)
    endif()
```

### 8.3 全量测试

```bash
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

集成测试会启动真实进程，约 10 秒。若失败，先清理端口占用：

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

## 10. 阶段 9：Release 构建与 v1.0.0 标签

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
ctest --test-dir build-release --output-on-failure
```

预期 `7/7` 通过。

打标签：

```bash
git tag -a v1.0.0 -m "EdgeGate v1.0.0 发布"
git show --stat v1.0.0 | head -30
```

查看完整历史：

```bash
git log --oneline --reverse
```

应该看到 9 个提交，从 `初始化 EdgeGate 仓库骨架` 到 `添加架构、教程和基准文档`。

---

## 11. GitHub：首次关联、每阶段推送与发布验证

> 推荐在完成阶段 1 后就创建空仓库并首次推送；之后每个阶段 `git commit` 后执行 `git push`。这样 GitHub 上始终有可恢复的进度。
>
> **如果你已经完成首次推送**（例如当前状态：`origin/main` 已经有阶段 0/1 两条 commit），跳过 11.1–11.2，直接从阶段 2 继续，每阶段结束 `git push` 即可。

### 11.1 创建空仓库

在浏览器打开 <https://github.com/new>：

- Owner：`yjwyyds1`（或你的账号）
- Repository name：`EdgeGate`
- Description：`C++20 HTTP/1.1 reverse proxy with epoll, connection pooling, backpressure and safe retries`
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

> 首次推送只推 `main`，**不要**在这里推 tag；`v1.0.0` 要等第 9 阶段所有 commit 完成后再打、再推。

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

### 11.4 发布 `v1.0.0` 标签

第 9 阶段 Release 构建和测试通过后：

> 如果第 10 节已经创建了 `v1.0.0`，跳过 `git tag -a`，直接执行两条 push；重复创建会提示 `tag already exists`。

```bash
cd ~/dev/EdgeGate
git tag -a v1.0.0 -m "EdgeGate v1.0.0 发布"
git show --stat v1.0.0 | head -30

git push origin main
git push origin v1.0.0
```

如果 tag 已存在但指错了 commit：

```bash
git tag -d v1.0.0
git tag -a v1.0.0 -m "EdgeGate v1.0.0 发布"
git push origin :refs/tags/v1.0.0
git push origin v1.0.0
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

- 9 个 commit；
- `v1.0.0` tag；
- README 正常渲染（Mermaid 图在 GitHub 上会显示）；
- 没有 `build/`、`build-release/`、`.o` 文件。

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

如果全新 clone 也能 `7/7` 通过，说明：没有漏提交文件、没有依赖本地 build 缓存、换行和脚本权限都正确。

### 11.7 可选：仓库信息

在 GitHub 仓库页面：

- 右侧 About → 编辑 Description、Website、Topics：
  - `cpp20`, `http-proxy`, `epoll`, `load-balancer`, `backpressure`, `systems-programming`
- 如果先建了 Private，确认内容可公开后再 `Settings → General → Change visibility → Public`。
- 如果要让别人复用代码，添加 LICENSE（MIT/Apache-2.0 等）；不添加则默认保留所有权利。

---

## 12. 常见问题

| 现象 | 处理 |
| --- | --- |
| `cmake: command not found` | `sudo apt install -y cmake ninja-build build-essential` |
| `ninja: command not found` | 同上，或 `cmake -G "Unix Makefiles"` |
| `libcrypto not found` | `sudo apt install -y libssl-dev`，然后删掉 `build/` 重新配置 |
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
| 6 | `实现 epoll 代理、管理端和安全重试` | epoll 代理、连接池、背压、管理端 |
| 7 | `添加文件/API 演示后端和压测器` | 真实文件/API 服务与压测器 |
| 8 | `添加端到端集成测试和脚本` | 18 个端到端场景 |
| 9 | `添加架构、教程和基准文档` | 架构、教程、设计与基准 |
| tag | `v1.0.0` | 可复现的发布点 |

**面试时可以按这个顺序讲**：先讲协议解析和事件循环，再讲连接池/健康/重试，最后讲背压和可观测性。每个 commit 都是一个可验证的里程碑。

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

# 5. 远端
git remote -v
git ls-remote --heads origin
git ls-remote --tags origin
```

全部通过后，你就可以：

- 用 `git clone` 在另一台机器复现；
- 在 GitHub 上展示 commit 历史和 tag；
- 按 [TUTORIAL.md](../TUTORIAL.md) 第 10 章的简历/面试提纲准备讲解；
- 继续做 TLS、HTTP/2 或 pack 等扩展。

---

## 附录 A：文件清单（按阶段）

| 阶段 | 新增文件 |
| --- | --- |
| 0 | `.gitignore`, `.gitattributes`, `README.md` |
| 1 | `include/edgegate/{buffer,util,log,event_loop}.hpp`, `src/{util,log}.cpp`, `tests/{test_util.hpp,buffer_test.cpp}`, `CMakeLists.txt` |
| 2 | `include/edgegate/http.hpp`, `src/http.cpp`, `tests/http_test.cpp` |
| 3 | `include/edgegate/config.hpp`, `src/config.cpp`, `tests/config_test.cpp` |
| 4 | `include/edgegate/{metrics,backend,retry}.hpp`, `src/{metrics,backend,retry}.cpp`, 3 个测试 |
| 5 | `include/edgegate/{proxy,admin}.hpp`, `src/{proxy,admin}.cpp`, `apps/edgegate.cpp`, `config/edgegate.conf` |
| 6 | `apps/{demo_backend,eg_loadgen}.cpp` |
| 7 | `tests/integration_test.py`, `scripts/*` |
| 8 | `README.md`（完整版）、`ARCHITECTURE.md`、`TUTORIAL.md`、`docs/*` |

## 附录 B：最短命令串（熟练后）

```bash
# 环境自检
cmake --version && c++ --version && git --version

# 建仓库
mkdir -p ~/dev/EdgeGate && cd ~/dev/EdgeGate && git init

# 之后按第 1–9 阶段执行；每阶段：
cmake --build build -j && ctest --test-dir build --output-on-failure
find include src tests apps -type f \( -name '*.hpp' -o -name '*.cpp' \) -exec chmod 644 {} + 2>/dev/null || true
chmod 644 config/*.conf 2>/dev/null || true
git add -A && git commit -m "..."
git push

# 发布
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j && ctest --test-dir build-release --output-on-failure
git tag -a v1.0.0 -m "EdgeGate v1.0.0 发布"

# 推送（首次在阶段 1 后执行 add remote / push -u；之后每阶段 git push）
git remote add origin git@github.com:yjwyyds1/EdgeGate.git
git branch -M main
git push -u origin main
git push origin v1.0.0
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
find include src tests apps -type f \( -name '*.hpp' -o -name '*.cpp' \) -exec chmod 644 {} +
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
git tag -d v1.0.0
git tag -a v1.0.0 -m "EdgeGate v1.0.0 发布"
git push origin :refs/tags/v1.0.0
git push origin v1.0.0
```

---

> 这份手册的目标不是“把文件复制过去”，而是让你在每一步都能回答：**我加了什么、为什么这样分层、测试证明了什么、下一个 commit 为什么存在。** 如果某一步想深入，回到 TUTORIAL 对应章节，而不是继续机械复制。
