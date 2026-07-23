# gst_opt/api.cmake — 通用版，无厂商专有库依赖
#
# 原版依赖灵眸（Easy EAI）专有静态库 libgst_opt.a。
# 新版改为从本目录的 gst_opt_impl.c 编译，仅使用标准 GStreamer 公共 API。
# 适用于正点原子、Radxa、PINE64 等任意 RK3588 平台。

find_package(PkgConfig REQUIRED)
pkg_search_module(GST1.0        REQUIRED gstreamer-1.0)
pkg_search_module(GST1.0_VIDEO  REQUIRED gstreamer-video-1.0)
pkg_search_module(GST1.0_SERVER REQUIRED gstreamer-rtsp-server-1.0)
pkg_search_module(GST1.0_ALLOCATORS REQUIRED gstreamer-allocators-1.0)

# 源文件（替代原先的 libgst_opt.a 预编译静态库）
file(GLOB GSTOPT_SOURCE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/*.c
    ${CMAKE_CURRENT_LIST_DIR}/*.cpp
)

# 头文件路径
set(GSTOPT_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}
    ${GST1.0_INCLUDE_DIRS}
    ${GST1.0_VIDEO_INCLUDE_DIRS}
    ${GST1.0_SERVER_INCLUDE_DIRS}
    ${GST1.0_ALLOCATORS_INCLUDE_DIRS}
)

# 链接库（移除 gst_opt 静态库，改为源码编译）
set(GSTOPT_LIBS
    ${GST1.0_LIBRARIES}
    ${GST1.0_VIDEO_LIBRARIES}
    ${GST1.0_SERVER_LIBRARIES}
    ${GST1.0_ALLOCATORS_LIBRARIES}
)

# 不再需要额外的库目录
set(GSTOPT_LIBS_DIRS "")
