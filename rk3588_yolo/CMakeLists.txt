cmake_minimum_required(VERSION 3.10.2)

STRING(REGEX REPLACE ".*/(.*)" "\\1" CURRENT_FOLDER ${CMAKE_CURRENT_SOURCE_DIR})
MESSAGE("current project: " ${CURRENT_FOLDER})

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_CROSSCOMPILING TRUE)

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

project(${CURRENT_FOLDER})
add_definitions(-Wall -Wpointer-arith -O3 -ffast-math)

# ======================== third_party helpers =========================
set(toolkit_root ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party)
set(common_root  ${toolkit_root})

if(EXISTS ${common_root}/system_opt/api.cmake)
    include(${common_root}/system_opt/api.cmake)
else()
    set(SYSOPT_INCLUDE_DIRS "")
    set(SYSOPT_SOURCE_DIRS "")
    set(SYSOPT_LIBS "")
endif()

if(EXISTS ${common_root}/gst_opt/api.cmake)
    include(${common_root}/gst_opt/api.cmake)
else()
    set(GSTOPT_INCLUDE_DIRS "")
    set(GSTOPT_SOURCE_DIRS "")
    set(GSTOPT_LIBS "")
    set(GSTOPT_LIBS_DIRS "")
endif()

set(api_inc
    ${SYSOPT_INCLUDE_DIRS}
    ${GSTOPT_INCLUDE_DIRS}
)

# 注意：GSTOPT_SOURCE_DIRS 现在包含 gst_opt_impl.c（替代原 libgst_opt.a）
file(GLOB api_srcs
    ${SYSOPT_SOURCE_DIRS}
    ${GSTOPT_SOURCE_DIRS}
)

link_directories(
    ${GSTOPT_LIBS_DIRS}
)

set(sysLib_list
    ${SYSOPT_LIBS}
    ${GSTOPT_LIBS}
)
# ======================== third_party helpers END =========================

# 兼容旧版 easyeai vendor 目录（如果存在，附加其源文件；通常新工程不会有此目录）
set(vendor_root ${PROJECT_SOURCE_DIR}/src/third_party/easyeai/common)
if(EXISTS ${vendor_root})
    list(APPEND api_inc ${vendor_root}/base64 ${vendor_root}/system_opt ${vendor_root}/gst_opt)
    file(GLOB VENDOR_BASE64_SRCS ${vendor_root}/base64/*.cpp)
    file(GLOB VENDOR_SYSOPT_SRCS ${vendor_root}/system_opt/*.cpp)
    list(APPEND api_srcs ${VENDOR_BASE64_SRCS} ${VENDOR_SYSOPT_SRCS})
endif()

# ========================== custom ============================
find_package(OpenCV REQUIRED)
find_package(PkgConfig REQUIRED)

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

# BLAS / LAPACK：源码中无直接调用，设为可选。
# OpenCV 已自带所需的矩阵运算后端，不依赖外部 BLAS。
# 若将来引入直接调用（cblas_sgemm 等），再改回 REQUIRED。
find_package(BLAS)
find_package(LAPACK)

pkg_check_modules(GTK3 REQUIRED gtk+-3.0)

set(custom_inc
    ${OpenCV_INCLUDE_DIRS}
    ${GTK3_INCLUDE_DIRS}
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/third_party/json
)

set(custom_libs
    rga
    rknnrt
    hiredis
    ${OpenCV_LIBS}
    ${GTK3_LIBRARIES}
    Threads::Threads
)

# 仅在找到时才链接，避免板端未安装时报错
if(BLAS_FOUND)
    list(APPEND custom_libs ${BLAS_LIBRARIES})
endif()
if(LAPACK_FOUND)
    list(APPEND custom_libs ${LAPACK_LIBRARIES})
endif()

aux_source_directory(${PROJECT_SOURCE_DIR}/src                          dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/config                   dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/core                     dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/capturer                 dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/analyzer                 dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/logic     dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/player                   dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/yolo                     dir_srcs)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/uploader                 dir_srcs)
set(cjson_src ${PROJECT_SOURCE_DIR}/src/third_party/json/cJSON.c)
# ========================== custom ============================

add_executable(rk3588_yolo ${dir_srcs} ${api_srcs} ${cjson_src})
target_include_directories(rk3588_yolo PRIVATE ${custom_inc} ${api_inc})
target_link_libraries(rk3588_yolo ${custom_libs} ${sysLib_list})
