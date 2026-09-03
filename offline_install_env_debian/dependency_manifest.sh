#!/usr/bin/env bash
# 项目第三方依赖的唯一清单。install_deps.sh 与离线包制作脚本共同读取此文件。
# 这里仅声明数据，不执行安装命令。

OFFLINE_DEPENDENCY_SCHEMA=3
DEFAULT_NODE_VERSION="v20.18.0"

# 这些脚本会参与离线包与目标项目的版本校验，避免新安装器调用旧验收/部署逻辑。
OFFLINE_PROJECT_FILES=(
    install_deps.sh
    offline_install_env_debian/dependency_manifest.sh
    vision_analysis/vendor/rockchip/PLATFORM_COMPATIBILITY.env
    web_console/install.sh
)

APT_RUNTIME=(
    # 下载、证书和 Python 包安装
    ca-certificates curl gnupg xz-utils
    python3 python3-pip python3-dev python3-setuptools python3-wheel python3-venv
    build-essential libffi-dev libssl-dev

    # Web 控制台实际调用的系统命令
    systemd dbus network-manager wpasupplicant iproute2 iputils-ping ethtool
    procps x11-xserver-utils tzdata
    ffmpeg v4l-utils

    # PAM 登录、GPIO、GTK/OpenCV 运行 ABI 与中文叠字字体
    libpam0g libgpiod2 libgtk-3-0
    libopencv-dev libopencv-contrib-dev
    fonts-wqy-zenhei

    # GStreamer 核心、RTSP server 以及项目用到的解析/编解码/封装插件
    libgstreamer1.0-0 libgstreamer-plugins-base1.0-0 libgstrtspserver-1.0-0
    gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good
    gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav
)

APT_BUILD=(
    cmake pkg-config binutils rsync git clang-format
    libgtk-3-dev libgpiod-dev
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
    libgstrtspserver-1.0-dev
    libblas-dev liblapack-dev
)

# 路径相对于项目根目录。测试夹具中的 requirements.txt 不属于生产环境。
PYTHON_REQUIREMENTS=(
    web_console/backend/requirements.txt
    service/model_update/requirements.txt
    service/upload/requirements.txt
)
