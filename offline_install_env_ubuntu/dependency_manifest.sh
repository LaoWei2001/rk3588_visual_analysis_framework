#!/usr/bin/env bash
# Ubuntu ARM64 环境与 Web 控制台离线包的基础依赖清单。

OFFLINE_DEPENDENCY_SCHEMA=11
DEFAULT_NODE_VERSION="v20.18.0"
MIN_NODE_MAJOR=18
NODE_INSTALL_PARENT="/usr/local/lib/nodejs"

APT_RUNTIME=(
    ca-certificates curl xz-utils
    python3 python3-pip python3-setuptools python3-wheel python3-cffi
    libc6 libstdc++6 binutils

    systemd dbus network-manager wpasupplicant iproute2 iputils-ping ethtool
    procps x11-xserver-utils tzdata
    ffmpeg v4l-utils

    libpam0g libgpiod2 libgtk-3-0
    fonts-wqy-zenhei

    libgstreamer1.0-0 libgstreamer-plugins-base1.0-0 libgstrtspserver-1.0-0
    gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good
    gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav
)

APT_BUILD=(
    build-essential cmake pkg-config rsync git clang-format
    python3-dev
    libffi-dev libssl-dev
    libgtk-3-dev libgpiod-dev
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
    libgstrtspserver-1.0-dev
    libopencv-dev libopencv-contrib-dev
)

# Ubuntu APT 源中通常不存在的 Rockchip BSP 厂商包。制作机必须已经安装这些包，
# 制包器会用 dpkg-repack 保存制作机上的准确版本。
LOCAL_RUNTIME_PACKAGES=(
    librga2
    librockchip-mpp1
    librockchip-vpu0
    gstreamer1.0-rockchip1
    libv4l-rkmpp
)

LOCAL_BUILD_PACKAGES=(
    librga-dev
    librockchip-mpp-dev
)

BUNDLED_RUNTIME_FILES=(
    "vision_analysis/vendor/rknn/2.4.2a2/lib/aarch64/librknnrt.so|/opt/vision-analysis/rockchip/lib/librknnrt.so"
    "vision_analysis/vendor/rknn/2.4.2a2/include/rknn_api.h|/opt/vision-analysis/rockchip/include/rknn_api.h"
)

PYTHON_REQUIREMENTS=(
    web_console/backend/requirements.txt
    service/model_update/requirements.txt
    service/upload/requirements.txt
)
