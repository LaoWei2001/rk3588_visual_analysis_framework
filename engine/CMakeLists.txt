cmake_minimum_required(VERSION 3.10.2)

# 导出 compile_commands.json：让 VS Code / clangd 的 IntelliSense 用与编译器完全相同的 -I（含 OpenCV），
# 从根上消除 cv:: 的红波浪线，且永远和真实编译保持一致（不用再往 third_party 拷 opencv2）。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

cmake_host_system_information(RESULT arch_value QUERY OS_PLATFORM)
message(STATUS "architecture: " ${arch_value})

# ===================== 交叉编译工具链（通用配置）=====================
# 仅在非 ARM 主机上需要交叉编译工具链。
# 查找优先级（由高到低）：
#   1. 命令行 -DCROSS_CMAKE_FILE=<path>
#   2. 环境变量 CROSS_CMAKE_FILE
# 如需在正点原子 / Radxa / PINE64 等板子上直接板端编译，无需任何工具链文件。
if((NOT "${arch_value}" STREQUAL "armv7l") AND (NOT "${arch_value}" STREQUAL "aarch64"))
    if(DEFINED CROSS_CMAKE_FILE AND EXISTS "${CROSS_CMAKE_FILE}")
        message(STATUS "Cross-compile toolchain (via -DCROSS_CMAKE_FILE): ${CROSS_CMAKE_FILE}")
        include("${CROSS_CMAKE_FILE}")
    elseif(DEFINED ENV{CROSS_CMAKE_FILE} AND EXISTS "$ENV{CROSS_CMAKE_FILE}")
        message(STATUS "Cross-compile toolchain (via CROSS_CMAKE_FILE env): $ENV{CROSS_CMAKE_FILE}")
        include("$ENV{CROSS_CMAKE_FILE}")
    else()
        message(WARNING
            "Cross-compile toolchain file not found!\n"
            "Please provide one via:\n"
            "  cmake -DCROSS_CMAKE_FILE=/path/to/cross.cmake ..\n"
            "  export CROSS_CMAKE_FILE=/path/to/cross.cmake"
        )
    endif()
endif()

# ===================== 交叉编译工具链 END =====================

project(vision_analysis LANGUAGES C CXX)
add_definitions(-Wall -Wpointer-arith -O3 -ffast-math)

# 上层业务工作区与底层引擎源码分离。二次开发者升级引擎时只需迁移整个
# projects/ 目录；也可通过 -DVISION_PROJECTS_ROOT=/path/to/projects 使用外部工作区。
set(VISION_PROJECTS_ROOT
    "${PROJECT_SOURCE_DIR}/../projects"
    CACHE PATH "Business logic, assets and project-owned services root"
)
get_filename_component(VISION_PROJECTS_ROOT "${VISION_PROJECTS_ROOT}" ABSOLUTE)
if(NOT EXISTS "${VISION_PROJECTS_ROOT}/modules" OR
   NOT EXISTS "${VISION_PROJECTS_ROOT}/global_modules" OR
   NOT EXISTS "${VISION_PROJECTS_ROOT}/catalog.json")
    message(FATAL_ERROR
        "业务工作区不完整: ${VISION_PROJECTS_ROOT}\n"
        "必须包含 modules/、global_modules/ 和 catalog.json")
endif()
message(STATUS "Business projects root: ${VISION_PROJECTS_ROOT}")
get_filename_component(VISION_REPOSITORY_ROOT "${PROJECT_SOURCE_DIR}/.." ABSOLUTE)
set(LOGIC_CATALOG_GENERATOR
    "${VISION_REPOSITORY_ROOT}/tools/build/generate_logics_catalog.py")
if(NOT EXISTS "${LOGIC_CATALOG_GENERATOR}")
    message(FATAL_ERROR "缺少 Logic 清单生成器: ${LOGIC_CATALOG_GENERATOR}")
endif()

# ======================== pinned RKNN Runtime =========================
# RKNN 的头文件和用户态 Runtime 随源码锁定，避免不同构建机上的
# /usr/include、ldconfig 或 sysroot 顺序生成内容不同的发布包。
set(RKNN_VENDOR_VERSION "2.4.2a2")
set(RKNN_VENDOR_ROOT
    "${PROJECT_SOURCE_DIR}/vendor/rknn/${RKNN_VENDOR_VERSION}"
    CACHE PATH "Pinned RKNN Runtime SDK root"
)
set(RKNN_API_INCLUDE_DIR "${RKNN_VENDOR_ROOT}/include")
set(RKNN_API_HEADER "${RKNN_API_INCLUDE_DIR}/rknn_api.h")
set(RKNN_RT_LIBRARY "${RKNN_VENDOR_ROOT}/lib/aarch64/librknnrt.so")
set(RKNN_API_HEADER_SHA256
    "61eedea2dd0fed8e4ee667fef571bf6b07fc87495bb6282d65055f60c61e7079")
set(RKNN_RT_LIBRARY_SHA256
    "bf50d51705ae433013927a13520ae781b534fdb1481c47bdddbc726f63ed4970")

foreach(required_file IN ITEMS "${RKNN_API_HEADER}" "${RKNN_RT_LIBRARY}")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR
            "缺少项目锁定的 RKNN SDK 文件: ${required_file}\n"
            "请恢复 vendor/rknn/${RKNN_VENDOR_VERSION}，禁止回退到系统 RKNN Runtime。")
    endif()
endforeach()

file(SHA256 "${RKNN_API_HEADER}" RKNN_API_HEADER_ACTUAL_SHA256)
if(NOT RKNN_API_HEADER_ACTUAL_SHA256 STREQUAL RKNN_API_HEADER_SHA256)
    message(FATAL_ERROR
        "rknn_api.h 校验失败。\n"
        "期望: ${RKNN_API_HEADER_SHA256}\n"
        "实际: ${RKNN_API_HEADER_ACTUAL_SHA256}")
endif()

file(SHA256 "${RKNN_RT_LIBRARY}" RKNN_RT_LIBRARY_ACTUAL_SHA256)
if(NOT RKNN_RT_LIBRARY_ACTUAL_SHA256 STREQUAL RKNN_RT_LIBRARY_SHA256)
    message(FATAL_ERROR
        "librknnrt.so 校验失败。\n"
        "期望: ${RKNN_RT_LIBRARY_SHA256}\n"
        "实际: ${RKNN_RT_LIBRARY_ACTUAL_SHA256}")
endif()

add_library(rknnrt_vendor SHARED IMPORTED GLOBAL)
set_target_properties(rknnrt_vendor PROPERTIES
    IMPORTED_LOCATION "${RKNN_RT_LIBRARY}"
    IMPORTED_SONAME "librknnrt.so"
    INTERFACE_INCLUDE_DIRECTORIES "${RKNN_API_INCLUDE_DIR}"
)
message(STATUS
    "Pinned RKNN Runtime: ${RKNN_VENDOR_VERSION} (${RKNN_RT_LIBRARY_SHA256})")
# ======================== pinned RKNN Runtime END =====================

# ======================== third_party helpers =========================
set(toolkit_root ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party)
set(common_root  ${toolkit_root})

if(EXISTS ${common_root}/gst_opt/api.cmake)
    include(${common_root}/gst_opt/api.cmake)
else()
    set(GSTOPT_INCLUDE_DIRS "")
    set(GSTOPT_SOURCE_DIRS "")
    set(GSTOPT_LIBS "")
endif()

set(api_inc
    ${GSTOPT_INCLUDE_DIRS}
)

# GStreamer 辅助实现直接随引擎源码编译。
file(GLOB api_srcs
    ${GSTOPT_SOURCE_DIRS}
)

set(sysLib_list
    ${GSTOPT_LIBS}
)
# ======================== third_party helpers END =========================

# ========================== custom ============================
find_package(OpenCV REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# 画面文字统一用 OpenCV freetype 模块渲染(中英文), 不再回退 Hershey。该模块为【必需】:
# 找不到 opencv2/freetype.hpp 直接报错, 而不是静默降级。opencv_freetype 库已含在 OpenCV_LIBS 中。
find_path(OPENCV_FREETYPE_HEADER opencv2/freetype.hpp PATHS ${OpenCV_INCLUDE_DIRS})
if(NOT OPENCV_FREETYPE_HEADER)
    message(FATAL_ERROR
        "未找到 OpenCV freetype 模块(opencv2/freetype.hpp)。\n"
        "本工程画面文字统一用 freetype 渲染(中文+英文), 该模块为必需。\n"
        "请安装带 contrib freetype 的 OpenCV(libopencv-contrib / 自编 opencv_contrib)。")
endif()
message(STATUS "OpenCV freetype module found -> overlay text rendered via FreeType (CN + EN)")

pkg_check_modules(GTK3 REQUIRED gtk+-3.0)

set(custom_inc
    ${OpenCV_INCLUDE_DIRS}
    ${GTK3_INCLUDE_DIRS}
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)

# Business code receives public headers and OpenCV only.
# No engine/src, RKNN, GStreamer or GTK include path is exported.
add_library(vision_sdk INTERFACE)
add_library(Vision::SDK ALIAS vision_sdk)
target_include_directories(vision_sdk INTERFACE
    ${PROJECT_SOURCE_DIR}/include
)
target_include_directories(vision_sdk SYSTEM INTERFACE ${OpenCV_INCLUDE_DIRS})
target_compile_features(vision_sdk INTERFACE cxx_std_11)

set(custom_libs
    rga
    rknnrt_vendor
    ${OpenCV_LIBS}
    ${GTK3_LIBRARIES}
    Threads::Threads
    gpiod
)

aux_source_directory(${PROJECT_SOURCE_DIR}/src                          dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/config                   dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/runtime                  dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/capturer                 dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/pipeline                 dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/inference                dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/tracking                 dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/control                  dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/dataset                  dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/gpio                     dir_srcs)
# 框架仅保留 src/logic/core；可自由组合的业务逻辑统一位于 projects/。
# 递归收集让新增/删除模块无需修改 CMakeLists.txt；重新运行 cmake 即可刷新列表。
file(GLOB_RECURSE logic_core_srcs
    "${PROJECT_SOURCE_DIR}/src/logic/core/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/logic/core/*.cc"
    "${PROJECT_SOURCE_DIR}/src/logic/core/*.cxx"
)
file(GLOB_RECURSE project_logic_srcs
    "${VISION_PROJECTS_ROOT}/modules/*.cpp"
    "${VISION_PROJECTS_ROOT}/modules/*.cc"
    "${VISION_PROJECTS_ROOT}/modules/*.cxx"
    "${VISION_PROJECTS_ROOT}/global_modules/*.cpp"
    "${VISION_PROJECTS_ROOT}/global_modules/*.cc"
    "${VISION_PROJECTS_ROOT}/global_modules/*.cxx"
)
list(APPEND dir_srcs ${logic_core_srcs})

# OBJECT keeps static registration constructors in the final executable.
# Business sources directly include the public SDK definitions.
if(project_logic_srcs)
    add_library(vision_business OBJECT ${project_logic_srcs})
    target_link_libraries(vision_business PRIVATE Vision::SDK)
endif()

# 模块 logic.json 是参数 Schema 的唯一真源。构建时将聚合后的能力清单嵌入二进制，
# 因此 --debug 直接运行也能完成参数默认值、类型和热重载策略校验，不依赖外部 logics.json。
find_program(PYTHON3_EXECUTABLE NAMES python3)
if(NOT PYTHON3_EXECUTABLE)
    message(FATAL_ERROR "生成逻辑参数能力需要 python3")
endif()
file(GLOB logic_manifest_files
    "${VISION_PROJECTS_ROOT}/modules/*/logic.json"
    "${VISION_PROJECTS_ROOT}/global_modules/*/logic.json"
)
file(GLOB_RECURSE logic_validation_files
    "${VISION_PROJECTS_ROOT}/modules/*.cpp"
    "${VISION_PROJECTS_ROOT}/modules/*.cc"
    "${VISION_PROJECTS_ROOT}/modules/*.cxx"
    "${VISION_PROJECTS_ROOT}/modules/*.h"
    "${VISION_PROJECTS_ROOT}/modules/*.hpp"
    "${VISION_PROJECTS_ROOT}/global_modules/*.cpp"
    "${VISION_PROJECTS_ROOT}/global_modules/*.cc"
    "${VISION_PROJECTS_ROOT}/global_modules/*.cxx"
    "${VISION_PROJECTS_ROOT}/global_modules/*.h"
    "${VISION_PROJECTS_ROOT}/global_modules/*.hpp"
)
set(logic_catalog_cpp "${CMAKE_CURRENT_BINARY_DIR}/generated/logic_catalog_embedded.cpp")
# 文件列表变化也要刷新清单，避免删除模块后仍沿用缓存的参数 Schema。
set(logic_catalog_inputs "${CMAKE_CURRENT_BINARY_DIR}/generated/logic_catalog_inputs.txt")
file(GENERATE OUTPUT "${logic_catalog_inputs}"
    CONTENT "${logic_manifest_files}\n${logic_validation_files}\n")
add_custom_command(
    OUTPUT "${logic_catalog_cpp}"
    COMMAND "${PYTHON3_EXECUTABLE}"
            "${LOGIC_CATALOG_GENERATOR}"
            --logic-root "${VISION_PROJECTS_ROOT}"
            --cpp-output "${logic_catalog_cpp}"
    DEPENDS
            "${logic_catalog_inputs}"
            "${LOGIC_CATALOG_GENERATOR}"
            "${VISION_PROJECTS_ROOT}/catalog.json"
            ${logic_manifest_files}
            ${logic_validation_files}
    COMMENT "Validating and embedding per-logic parameter schemas"
    VERBATIM
)
list(APPEND dir_srcs "${logic_catalog_cpp}")
aux_source_directory(${PROJECT_SOURCE_DIR}/src/display                  dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/rtsp                     dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/yolo                     dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/recorder                 dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/event                    dir_srcs)
set(cjson_src ${PROJECT_SOURCE_DIR}/src/third_party/json/cJSON.c)
# ========================== custom ============================

add_executable(vision_analysis ${dir_srcs} ${api_srcs} ${cjson_src})
if(TARGET vision_business)
    target_sources(vision_analysis PRIVATE $<TARGET_OBJECTS:vision_business>)
endif()
target_include_directories(vision_analysis PRIVATE ${custom_inc} ${api_inc})
target_link_libraries(vision_analysis ${custom_libs} ${sysLib_list})

option(BUILD_SDK_TESTS "Validate public SDK without engine include paths" OFF)
if(BUILD_SDK_TESTS)
    enable_testing()
    set(sdk_test_args
        --compiler "${CMAKE_CXX_COMPILER}"
        --include "${PROJECT_SOURCE_DIR}/include"
        --projects "${VISION_PROJECTS_ROOT}"
    )
    foreach(directory IN LISTS OpenCV_INCLUDE_DIRS)
        list(APPEND sdk_test_args --opencv "${directory}")
    endforeach()
    add_test(NAME sdk_public_headers_regression
        COMMAND "${PYTHON3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/sdk/test_public_headers.py" ${sdk_test_args})
    set_tests_properties(sdk_public_headers_regression PROPERTIES TIMEOUT 180)

    add_library(sdk_target_query_consumer OBJECT tests/sdk/test_target_query.cpp)
    target_link_libraries(sdk_target_query_consumer PRIVATE Vision::SDK)
    target_compile_options(sdk_target_query_consumer PRIVATE -UNDEBUG)
    add_executable(sdk_target_query_tests
        $<TARGET_OBJECTS:sdk_target_query_consumer>
        src/logic/core/channel_logic.cpp
    )
    target_include_directories(sdk_target_query_tests PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(sdk_target_query_tests PRIVATE -ffunction-sections -fdata-sections)
    set_target_properties(sdk_target_query_tests PROPERTIES LINK_FLAGS "-Wl,--gc-sections")
    target_link_libraries(sdk_target_query_tests PRIVATE ${OpenCV_LIBS} Threads::Threads)
    add_test(NAME sdk_target_query_regression COMMAND sdk_target_query_tests)
    set_tests_properties(sdk_target_query_regression PROPERTIES TIMEOUT 15)

    # The example is optional in externally supplied business workspaces.
    set(sdk_example "${VISION_PROJECTS_ROOT}/modules/logic_roi_dwell_demo/logic.cpp")
    if(EXISTS "${sdk_example}")
        add_library(sdk_example_consumer OBJECT "${sdk_example}")
        target_link_libraries(sdk_example_consumer PRIVATE Vision::SDK)
        add_executable(sdk_business_tests
            tests/sdk/test_business_workflow.cpp
            $<TARGET_OBJECTS:sdk_example_consumer>
            src/logic/core/channel_logic.cpp
            src/logic/core/logic_parameters.cpp
            src/display/display.cpp
            src/display/text_overlay.cpp
            "${logic_catalog_cpp}"
            ${cjson_src}
        )
        target_include_directories(sdk_business_tests PRIVATE ${custom_inc} ${api_inc})
        target_compile_options(sdk_business_tests PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
        set_target_properties(sdk_business_tests PROPERTIES LINK_FLAGS "-Wl,--gc-sections")
        target_link_libraries(sdk_business_tests PRIVATE ${OpenCV_LIBS} ${GTK3_LIBRARIES} Threads::Threads)
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sdk-acceptance")
        add_test(NAME sdk_business_workflow_regression
            COMMAND sdk_business_tests "${CMAKE_CURRENT_BINARY_DIR}/sdk-acceptance")
        set_tests_properties(sdk_business_workflow_regression PROPERTIES TIMEOUT 30)
    endif()
endif()

# 发布包即使不经过 run.sh 启动，也优先从可执行文件旁的 libs/ 加载依赖。
# BUILD_WITH_INSTALL_RPATH 避免把构建机上的 vendor 绝对路径写入最终 ELF。
set_target_properties(vision_analysis PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN/libs"
)

# 推流回归测试使用真实 GStreamer 队列验证慢消费者，不依赖摄像头或 NPU。
option(BUILD_RTSP_TESTS "Build live preview backpressure regression tests" OFF)
if(BUILD_RTSP_TESTS)
    enable_testing()
    add_executable(rtsp_live_source_tests tests/rtsp/test_live_source.cpp)
    target_include_directories(rtsp_live_source_tests PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(rtsp_live_source_tests PRIVATE -UNDEBUG)
    target_link_libraries(rtsp_live_source_tests PRIVATE ${GSTOPT_LIBS} Threads::Threads)
    add_test(NAME rtsp_live_source_regression COMMAND rtsp_live_source_tests)
    set_tests_properties(rtsp_live_source_regression PROPERTIES TIMEOUT 10)
endif()

# 跟踪器回归测试不依赖 RKNN/GStreamer 运行时，默认不纳入产品构建。
option(BUILD_TRACKER_TESTS "Build standalone tracker regression tests" OFF)
if(BUILD_TRACKER_TESTS)
    enable_testing()
    add_executable(tracker_tests
        tests/tracking/test_tracker.cpp
        src/tracking/tracker.cpp
    )
    target_include_directories(tracker_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${OpenCV_INCLUDE_DIRS}
        ${PROJECT_SOURCE_DIR}/src
    )
    target_link_libraries(tracker_tests PRIVATE ${OpenCV_LIBS})
    add_test(NAME tracker_regression COMMAND tracker_tests)

    add_executable(bytetrack_tests
        tests/tracking/test_bytetrack.cpp
        src/tracking/bytetrack.cpp
    )
    target_include_directories(bytetrack_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${OpenCV_INCLUDE_DIRS}
        ${PROJECT_SOURCE_DIR}/src
    )
    target_link_libraries(bytetrack_tests PRIVATE ${OpenCV_LIBS})
    add_test(NAME bytetrack_regression COMMAND bytetrack_tests)
endif()

option(BUILD_INFERENCE_ROI_TESTS "Build standalone inference ROI filter tests" OFF)
if(BUILD_INFERENCE_ROI_TESTS)
    enable_testing()
    add_executable(inference_roi_filter_tests
        tests/inference/test_inference_roi_filter.cpp
        src/inference/inference_roi_filter.cpp
        src/inference/inference_roi_geometry.cpp
    )
    target_include_directories(inference_roi_filter_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${OpenCV_INCLUDE_DIRS}
        ${PROJECT_SOURCE_DIR}/src
    )
    target_compile_options(inference_roi_filter_tests PRIVATE -UNDEBUG)
    target_link_libraries(inference_roi_filter_tests PRIVATE ${OpenCV_LIBS})
    add_test(NAME inference_roi_filter_regression COMMAND inference_roi_filter_tests)

    add_executable(business_coordinates_tests
        tests/inference/test_business_coordinates.cpp
        src/inference/inference_result_mapping.cpp
        src/inference/inference_roi_geometry.cpp
        src/inference/inference_roi_filter.cpp
        src/pipeline/frame_transform.cpp
        src/pipeline/image_convert.cpp
        src/logic/core/channel_logic.cpp
        src/display/display.cpp
        src/display/text_overlay.cpp
    )
    target_include_directories(business_coordinates_tests PRIVATE ${custom_inc} ${api_inc} ${RKNN_API_INCLUDE_DIR})
    target_compile_options(business_coordinates_tests PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
    set_target_properties(business_coordinates_tests PROPERTIES LINK_FLAGS "-Wl,--gc-sections")
    target_link_libraries(business_coordinates_tests PRIVATE ${OpenCV_LIBS} ${GSTOPT_LIBS} rga Threads::Threads)
    add_test(NAME business_coordinates_regression COMMAND business_coordinates_tests)

    add_executable(model_status_tests
        tests/inference/test_model_status.cpp
        src/inference/model_status.cpp
        src/yolo/composite_model.cpp
        ${cjson_src}
    )
    target_include_directories(model_status_tests PRIVATE ${custom_inc})
    target_compile_options(model_status_tests PRIVATE -UNDEBUG)
    target_link_libraries(model_status_tests PRIVATE ${OpenCV_LIBS} Threads::Threads)
    add_test(NAME model_status_regression COMMAND model_status_tests)
endif()

option(BUILD_FRAME_SNAPSHOT_TESTS "Build frame evidence and buffer ownership regression tests" OFF)
if(BUILD_FRAME_SNAPSHOT_TESTS)
    enable_testing()
    add_executable(event_retention_tests tests/event/test_event_retention.cpp ${cjson_src})
    target_include_directories(event_retention_tests PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src)
    target_compile_options(event_retention_tests PRIVATE -UNDEBUG)
    add_test(NAME event_retention_regression COMMAND event_retention_tests)
    add_executable(frame_snapshot_tests
        tests/event/test_frame_snapshot.cpp
        src/runtime/channel_snapshot.cpp
        src/pipeline/frame_transform.cpp
        src/pipeline/image_convert.cpp
        src/display/display.cpp
        src/display/text_overlay.cpp
    )
    target_include_directories(frame_snapshot_tests PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(frame_snapshot_tests PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
    # 仅链接公共绘制函数，未使用的 GUI/实时播放入口不进入独立测试。
    set_target_properties(frame_snapshot_tests PROPERTIES LINK_FLAGS "-Wl,--gc-sections")
    target_link_libraries(frame_snapshot_tests PRIVATE ${OpenCV_LIBS} ${GSTOPT_LIBS} rga Threads::Threads)
    add_test(NAME frame_snapshot_regression COMMAND frame_snapshot_tests)
endif()

option(BUILD_DATASET_TESTS "Build dataset rule and remote collection regression tests" OFF)
if(BUILD_DATASET_TESTS)
    enable_testing()
    add_executable(dataset_rules_tests
        tests/dataset/test_rules.cpp
        src/dataset/rules.cpp
        ${cjson_src}
    )
    target_include_directories(dataset_rules_tests PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(dataset_rules_tests PRIVATE -UNDEBUG)
    target_link_libraries(dataset_rules_tests PRIVATE ${OpenCV_LIBS} Threads::Threads)
    add_test(NAME dataset_rules_regression COMMAND dataset_rules_tests)
    add_executable(remote_dataset_tests tests/dataset/test_remote_dataset.cpp
        src/control/remote_dataset.cpp
        src/dataset/rules.cpp ${cjson_src})
    target_include_directories(remote_dataset_tests PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(remote_dataset_tests PRIVATE -UNDEBUG)
    target_link_libraries(remote_dataset_tests PRIVATE ${OpenCV_LIBS} Threads::Threads)
    add_test(NAME remote_dataset_regression COMMAND remote_dataset_tests)
    set_tests_properties(remote_dataset_regression PROPERTIES TIMEOUT 20)
endif()

option(BUILD_HELMET_TESTS "Build helmet evidence and alarm mode regression tests" OFF)
if(BUILD_HELMET_TESTS)
    enable_testing()
    add_executable(helmet_tests
        tests/helmet/test_helmet_guard.cpp
        ${VISION_PROJECTS_ROOT}/modules/logic_crane_helmet/helmet_guard.cpp
    )
    target_include_directories(helmet_tests PRIVATE ${custom_inc} ${api_inc}
        ${VISION_PROJECTS_ROOT}/modules/logic_crane_helmet)
    target_compile_options(helmet_tests PRIVATE -UNDEBUG)
    target_link_libraries(helmet_tests PRIVATE ${OpenCV_LIBS})
    add_test(NAME helmet_regression COMMAND helmet_tests)
endif()

option(BUILD_PERFORMANCE_TESTS "Build standalone performance counter regression tests" OFF)
if(BUILD_PERFORMANCE_TESTS)
    enable_testing()
    add_executable(performance_metrics_tests tests/performance/test_performance_metrics.cpp)
    target_include_directories(performance_metrics_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_options(performance_metrics_tests PRIVATE -UNDEBUG)
    target_link_libraries(performance_metrics_tests PRIVATE Threads::Threads)
    add_test(NAME performance_metrics_regression COMMAND performance_metrics_tests)

    add_executable(display_metrics_tests
        tests/performance/test_display_metrics.cpp
        src/display/display_render.cpp
        src/display/display.cpp
        src/display/text_overlay.cpp
        src/runtime/pause_ctrl.cpp
    )
    target_include_directories(display_metrics_tests PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(display_metrics_tests PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
    set_target_properties(display_metrics_tests PROPERTIES LINK_FLAGS "-Wl,--gc-sections")
    target_link_libraries(display_metrics_tests PRIVATE ${OpenCV_LIBS} ${GSTOPT_LIBS} Threads::Threads)
    add_test(NAME display_metrics_regression COMMAND display_metrics_tests)
endif()

option(BUILD_PERFORMANCE_BENCHMARKS "Build RK3588 hardware display microbenchmark" OFF)
if(BUILD_PERFORMANCE_BENCHMARKS)
    add_executable(display_performance_benchmark
        tests/performance/benchmark_display.cpp
        src/display/display_render.cpp
        src/display/display.cpp
        src/display/text_overlay.cpp
        src/runtime/pause_ctrl.cpp
        src/pipeline/frame_transform.cpp
        src/pipeline/image_convert.cpp
    )
    target_include_directories(display_performance_benchmark PRIVATE ${custom_inc} ${api_inc})
    target_compile_options(display_performance_benchmark PRIVATE -ffunction-sections -fdata-sections)
    set_target_properties(display_performance_benchmark PROPERTIES LINK_FLAGS "-Wl,--gc-sections")
    target_link_libraries(display_performance_benchmark PRIVATE ${OpenCV_LIBS} ${GSTOPT_LIBS} rga Threads::Threads)
endif()
