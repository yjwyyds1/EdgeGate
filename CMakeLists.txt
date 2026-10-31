cmake_minimum_required(VERSION 3.22)
project(EdgeGate VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
    message(FATAL_ERROR "EdgeGate uses Linux epoll; build it inside WSL2 or a Linux host.")
endif()

find_package(Threads REQUIRED)
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
add_library(edgegate_core
    src/util.cpp
    src/log.cpp
    src/http.cpp
    src/config.cpp
    src/metrics.cpp
    src/backend.cpp
    src/retry.cpp
    src/admin.cpp
    src/proxy.cpp
)
target_include_directories(edgegate_core PUBLIC include)
target_compile_options(edgegate_core PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(edgegate_core PUBLIC Threads::Threads)

include(CTest)

add_executable(http_test tests/http_test.cpp)
target_link_libraries(http_test PRIVATE edgegate_core)
target_compile_options(http_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME http_test COMMAND http_test)

add_executable(edgegate apps/edgegate.cpp)
target_link_libraries(edgegate PRIVATE edgegate_core)
target_compile_options(edgegate PRIVATE -Wall -Wextra -Wpedantic)

add_executable(eg_demo apps/demo_backend.cpp)
target_link_libraries(eg_demo PRIVATE edgegate_core ${CRYPTO_LIBRARY})

add_executable(eg_loadgen apps/eg_loadgen.cpp)
target_link_libraries(eg_loadgen PRIVATE edgegate_core)

if(BUILD_TESTING)
    add_executable(buffer_test tests/buffer_test.cpp)
    target_link_libraries(buffer_test PRIVATE edgegate_core)
    target_compile_options(buffer_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME buffer_test COMMAND buffer_test)
    
    add_executable(config_test tests/config_test.cpp)
    target_link_libraries(config_test PRIVATE edgegate_core)
    target_compile_options(config_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME config_test COMMAND config_test)

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
    
endif()
