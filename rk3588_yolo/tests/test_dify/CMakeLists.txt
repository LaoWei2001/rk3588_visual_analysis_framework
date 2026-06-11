cmake_minimum_required(VERSION 3.10)
project(rk3588_dify_tests CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED)

add_executable(dify_queue_producer_test
    dify_queue_producer_test.cpp
    ../../src/uploader/alarm_uploader.cpp
    ../../src/core/base64_util.cpp
    ../../src/third_party/json/cJSON.c
)

target_include_directories(dify_queue_producer_test PRIVATE
    ../../src
    ../../src/third_party/json
)

# SOURCE_ROOT 宏：指向工程根目录（rk3588_multistream-v1.0/）
# 用于在代码中自动定位 service/upload/config.yaml，无需手动传 --config
get_filename_component(_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
target_compile_definitions(dify_queue_producer_test PRIVATE
    SOURCE_ROOT="${_REPO_ROOT}"
)

target_link_libraries(dify_queue_producer_test
    ${OpenCV_LIBS}
    hiredis
    pthread
)
