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

# SOURCE_ROOT 宏：指向工程根目录, 用于在代码中自动定位 service/upload/config.yaml,
# 无需手动传 --config（与 test_dify 保持一致）。
get_filename_component(_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
target_compile_definitions(server_queue_producer_test PRIVATE
    SOURCE_ROOT="${_REPO_ROOT}"
)

target_link_libraries(server_queue_producer_test
    ${OpenCV_LIBS}
    hiredis
    pthread
)
