#!/usr/bin/env bash
# 环境与 Web 控制台离线包的基础依赖清单。能从已构建 ELF 自动识别的库无需重复填写；
# 脚本、外部命令和尚未构建的新模块无法可靠推断时，可写入 extra-*-packages.txt。

OFFLINE_DEPENDENCY_SCHEMA=11
DEFAULT_NODE_VERSION="v20.18.0"
MIN_NODE_MAJOR=18
NODE_INSTALL_PARENT="/usr/local/lib/nodejs"

# Debian 11 安全仓库结束常规维护后，索引与 pool 文件可能在归档切换期间不同步。
# 固定到官方 snapshot 可确保同一批安全更新始终能够重新下载。
DEBIAN_SECURITY_SNAPSHOT="20260901T000000Z"

APT_RUNTIME=(
    ca-certificates curl xz-utils
    python3 python3-pip python3-setuptools python3-wheel libc6 libstdc++6

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
    build-essential cmake pkg-config binutils rsync git clang-format
    python3-dev
    libffi-dev libssl-dev
    libgtk-3-dev libgpiod-dev
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
    libgstrtspserver-1.0-dev
    libopencv-dev libopencv-contrib-dev
)

# 开发机已由 dpkg 管理、但配置的软件源中没有的瑞芯微用户态包。
# 制包时只对这些本地包使用 dpkg-repack；Debian 官方包始终按 APT 候选版本下载。
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

# 不属于任何 dpkg 包、但需要随空白 Debian 环境安装的固定文件。
# 格式：项目根目录相对路径|deb 内绝对安装路径
BUNDLED_RUNTIME_FILES=(
    "vision_analysis/vendor/rknn/2.4.2a2/lib/aarch64/librknnrt.so|/opt/vision-analysis/rockchip/lib/librknnrt.so"
    "vision_analysis/vendor/rknn/2.4.2a2/include/rknn_api.h|/opt/vision-analysis/rockchip/include/rknn_api.h"
)

# 路径相对于项目根目录。新增生产 requirements.txt 后在这里加一项。
PYTHON_REQUIREMENTS=(
    web_console/backend/requirements.txt
    service/model_update/requirements.txt
    service/upload/requirements.txt
)
