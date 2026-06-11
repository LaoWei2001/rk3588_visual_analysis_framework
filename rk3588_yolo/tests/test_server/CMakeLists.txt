cmake_minimum_required(VERSION 3.10)
project(rk3588_server_tests CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── 板端原生编译 or 交叉编译（复用主工程的工具链检测逻辑）──────────────────
# 若在 RK3588 板子上直接运行 cmake，不需要下面任何工具链配置。
# 若在 x86 服务器上交叉编译（一般不需要），传入：
#   cmake -DCROSS_CMAKE_FILE=/path/to/cross.cmake ..
cmake_host_system_information(RESULT _host_arch QUERY OS_PLATFORM)
if((NOT "${_host_arch}" STREQUAL "aarch64") AND (NOT "${_host_arch}" STREQUAL "armv7l"))
    if(DEFINED CROSS_CMAKE_FILE AND EXISTS "${CROSS_CMAKE_FILE}")
        include("${CROSS_CMAKE_FILE}")
    endif()
endif()

find_package(OpenCV REQUIRED)

add_executable(server_queue_producer_test
    server_queue_producer_test.cpp
    ../../src/uploader/alarm_uploader.cpp
    ../../src/core/base64_util.cpp
    ../../src/third_party/json/cJSON.c
)

target_include_directories(server_queue_producer_test PRIVATE
    ../../src
    ../../src/third_party/json
)

target_link_libraries(server_queue_producer_test
    ${OpenCV_LIBS}
    hiredis
    pthread
)
