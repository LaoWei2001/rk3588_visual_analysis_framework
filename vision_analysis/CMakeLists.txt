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
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/third_party/json
)

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
aux_source_directory(${PROJECT_SOURCE_DIR}/src/gpio                     dir_srcs)
# 通道逻辑位于 src/logic/modules/<logic_name>/，全局逻辑位于
# src/logic/global_modules/<logic_name>/，框架位于 src/logic/core/。
# 递归收集让新增/删除逻辑模块无需再修改 CMakeLists.txt；重新运行 cmake 即可刷新列表。
file(GLOB_RECURSE logic_srcs
    "${PROJECT_SOURCE_DIR}/src/logic/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/logic/*.cc"
    "${PROJECT_SOURCE_DIR}/src/logic/*.cxx"
)
list(APPEND dir_srcs ${logic_srcs})

# 模块 logic.json 是参数 Schema 的唯一真源。构建时将聚合后的能力清单嵌入二进制，
# 因此 --debug 直接运行也能完成参数默认值、类型和热重载策略校验，不依赖外部 logics.json。
find_program(PYTHON3_EXECUTABLE NAMES python3)
if(NOT PYTHON3_EXECUTABLE)
    message(FATAL_ERROR "生成逻辑参数能力需要 python3")
endif()
file(GLOB logic_manifest_files
    "${PROJECT_SOURCE_DIR}/src/logic/modules/*/logic.json"
    "${PROJECT_SOURCE_DIR}/src/logic/global_modules/*/logic.json"
)
file(GLOB_RECURSE logic_validation_files
    "${PROJECT_SOURCE_DIR}/src/logic/modules/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/logic/modules/*.cc"
    "${PROJECT_SOURCE_DIR}/src/logic/modules/*.cxx"
    "${PROJECT_SOURCE_DIR}/src/logic/modules/*.h"
    "${PROJECT_SOURCE_DIR}/src/logic/modules/*.hpp"
    "${PROJECT_SOURCE_DIR}/src/logic/global_modules/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/logic/global_modules/*.cc"
    "${PROJECT_SOURCE_DIR}/src/logic/global_modules/*.cxx"
    "${PROJECT_SOURCE_DIR}/src/logic/global_modules/*.h"
    "${PROJECT_SOURCE_DIR}/src/logic/global_modules/*.hpp"
)
set(logic_catalog_cpp "${CMAKE_CURRENT_BINARY_DIR}/generated/logic_catalog_embedded.cpp")
add_custom_command(
    OUTPUT "${logic_catalog_cpp}"
    COMMAND "${PYTHON3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/scripts/generate_logics_catalog.py"
            --logic-root "${PROJECT_SOURCE_DIR}/src/logic"
            --cpp-output "${logic_catalog_cpp}"
    DEPENDS
            "${PROJECT_SOURCE_DIR}/scripts/generate_logics_catalog.py"
            "${PROJECT_SOURCE_DIR}/src/logic/catalog.json"
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
target_include_directories(vision_analysis PRIVATE ${custom_inc} ${api_inc})
target_link_libraries(vision_analysis ${custom_libs} ${sysLib_list})

# 发布包即使不经过 run.sh 启动，也优先从可执行文件旁的 libs/ 加载依赖。
# BUILD_WITH_INSTALL_RPATH 避免把构建机上的 vendor 绝对路径写入最终 ELF。
set_target_properties(vision_analysis PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN/libs"
)
