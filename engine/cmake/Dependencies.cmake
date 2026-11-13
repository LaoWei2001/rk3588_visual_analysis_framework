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
set(toolkit_root ${PROJECT_SOURCE_DIR}/vendor)
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


set(custom_libs
    rga
    rknnrt_vendor
    ${OpenCV_LIBS}
    ${GTK3_LIBRARIES}
    Threads::Threads
    gpiod
)


set(cjson_src "${PROJECT_SOURCE_DIR}/vendor/cjson/cJSON.c")
