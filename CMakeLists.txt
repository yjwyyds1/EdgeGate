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
