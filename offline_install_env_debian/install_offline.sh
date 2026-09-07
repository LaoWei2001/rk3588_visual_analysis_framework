#!/usr/bin/env bash
# 从 create_bundle.sh 生成的本地 APT 仓库一键安装项目环境。
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUNDLE_DIR="$SCRIPT_DIR"

# 项目顶层脚本只负责转发到最新生成的 output/bundle。
if [ ! -f "$BUNDLE_DIR/BUNDLE_INFO" ] || [ ! -f "$BUNDLE_DIR/apt/Packages" ]; then
    PAYLOAD_DIR="$SCRIPT_DIR/output/bundle"
    if [ -f "$PAYLOAD_DIR/BUNDLE_INFO" ] \
            && [ -f "$PAYLOAD_DIR/apt/Packages" ] \
            && [ -f "$PAYLOAD_DIR/install_offline.sh" ]; then
        echo ">>> 使用离线仓库: $PAYLOAD_DIR"
        exec bash "$PAYLOAD_DIR/install_offline.sh" "$@"
    fi
    echo "[错误] 尚未生成离线仓库: $PAYLOAD_DIR" >&2
    echo "       请先在有网 ARM64 开发机运行 create_bundle.sh。" >&2
    exit 1
fi

WANT_BUILD=true
while [ "$#" -gt 0 ]; do
    case "$1" in
        --build) WANT_BUILD=true ;;
        --runtime-only) WANT_BUILD=false ;;
        -h|--help)
            echo "用法：sudo bash install_offline.sh [--runtime-only]"
            echo "默认安装运行环境和 C/C++ 编译环境；--runtime-only 只安装运行环境。"
            exit 0
            ;;
        *) echo "[错误] 未知参数: $1" >&2; exit 2 ;;
    esac
    shift
done

if [ "$(id -u)" -eq 0 ]; then
    ROOT=()
elif command -v sudo >/dev/null 2>&1; then
    ROOT=(sudo)
else
    echo "[错误] 安装 deb 需要 root 或 sudo。" >&2
    exit 1
fi
as_root() { "${ROOT[@]}" "$@"; }

metadata_value() {
    local key="$1"
    sed -n "s/^${key}=//p" "$BUNDLE_DIR/BUNDLE_INFO" | head -n 1
}

EXPECTED_ARCH="$(metadata_value deb_arch)"
CURRENT_ARCH="$(dpkg --print-architecture)"
if [ "$CURRENT_ARCH" != "$EXPECTED_ARCH" ]; then
    echo "[错误] deb 架构不匹配：离线仓库=$EXPECTED_ARCH，当前设备=$CURRENT_ARCH" >&2
    exit 1
fi

EXPECTED_OS="$(metadata_value os_id) $(metadata_value os_version_id)"
# shellcheck source=/etc/os-release
source /etc/os-release
CURRENT_OS="${ID:-unknown} ${VERSION_ID:-unknown}"
if [ "$CURRENT_OS" != "$EXPECTED_OS" ]; then
    echo "[警告] 制作机系统为 $EXPECTED_OS，当前设备为 $CURRENT_OS。" >&2
    echo "       将继续交给 APT 判断各 deb 是否兼容。" >&2
fi

RUNTIME_META="$(metadata_value runtime_meta_package)"
BUILD_META="$(metadata_value build_meta_package)"
TARGET_PACKAGE="$RUNTIME_META"
if [ "$WANT_BUILD" = true ]; then
    if [ -z "$BUILD_META" ]; then
        echo "[错误] 当前仓库没有编译环境，请重新运行默认的 create_bundle.sh。" >&2
        exit 1
    fi
    TARGET_PACKAGE="$BUILD_META"
fi

if [[ "$BUNDLE_DIR" =~ [[:space:]] ]]; then
    echo "[错误] 离线仓库路径不能包含空格: $BUNDLE_DIR" >&2
    exit 1
fi

APT_WORK_DIR="$(mktemp -d)"
cleanup() { rm -rf -- "$APT_WORK_DIR"; }
trap cleanup EXIT
mkdir -p "$APT_WORK_DIR/lists/partial" "$APT_WORK_DIR/cache/archives/partial" \
    "$APT_WORK_DIR/sourceparts"
printf 'deb [trusted=yes] file:%s/apt ./\n' "$BUNDLE_DIR" > "$APT_WORK_DIR/sources.list"

APT_OPTIONS=(
    -o APT::Sandbox::User=root
    -o "Dir::Etc::sourcelist=$APT_WORK_DIR/sources.list"
    -o "Dir::Etc::sourceparts=$APT_WORK_DIR/sourceparts"
    -o "Dir::State::lists=$APT_WORK_DIR/lists"
    -o "Dir::Cache::archives=$APT_WORK_DIR/cache/archives"
    -o Acquire::Languages=none
    -o Acquire::Retries=0
)

echo ">>> [1/2] 读取本地 APT 仓库..."
as_root apt-get "${APT_OPTIONS[@]}" update

echo ">>> [2/2] 安装 $TARGET_PACKAGE 及其全部依赖..."
as_root env DEBIAN_FRONTEND=noninteractive apt-get "${APT_OPTIONS[@]}" \
    --no-install-recommends -y install "$TARGET_PACKAGE"

echo
echo "[OK] 离线环境安装完成。"
echo "     Python 环境: /opt/vision-analysis/python-env"
echo "     前端产物: /usr/share/vision-analysis/frontend/dist"
echo "     RKNN 用户态库: /opt/vision-analysis/rockchip/lib"
echo "     安装 Web 控制台: sudo bash web_console/install.sh offline"
