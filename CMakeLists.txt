cmake_minimum_required(VERSION 3.22)
project(EdgeGate VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
    message(FATAL_ERROR "EdgeGate uses Linux epoll; build it inside WSL2 or a Linux host.")
endif()

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

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

add_library(edgegate_core
    src/util.cpp
    src/log.cpp
    src/tls.cpp
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
target_link_libraries(edgegate_core PUBLIC Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
if(EDGEGATE_HAVE_NGHTTP2)
    target_include_directories(edgegate_core PRIVATE ${NGHTTP2_INCLUDE_DIR})
    target_link_libraries(edgegate_core PUBLIC ${NGHTTP2_LIBRARY})
    target_compile_definitions(edgegate_core PUBLIC EDGEGATE_HAVE_NGHTTP2=1)
endif()

add_executable(edgegate apps/edgegate.cpp)
target_link_libraries(edgegate PRIVATE edgegate_core)

add_executable(eg_demo apps/demo_backend.cpp)
target_link_libraries(eg_demo PRIVATE edgegate_core)

add_executable(eg_loadgen apps/eg_loadgen.cpp)
target_link_libraries(eg_loadgen PRIVATE edgegate_core)

include(CTest)
if(BUILD_TESTING)
    foreach(test_name buffer_test http_test config_test balancer_test retry_test metrics_test)
        add_executable(${test_name} tests/${test_name}.cpp)
        target_link_libraries(${test_name} PRIVATE edgegate_core)
        target_compile_options(${test_name} PRIVATE -Wall -Wextra -Wpedantic)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME integration_test
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration_test.py
                         --binary-dir ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(integration_test PROPERTIES TIMEOUT 300)
        # 避免 WSL 继承的 Windows 代理把 127.0.0.1 也代理出去
        set_tests_properties(integration_test PROPERTIES ENVIRONMENT "no_proxy=*;NO_PROXY=*")
    endif()
endif()

