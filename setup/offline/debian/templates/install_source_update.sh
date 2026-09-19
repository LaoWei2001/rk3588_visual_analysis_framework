#!/usr/bin/env bash
# 制包器复制到 source-update 根目录；不要从 templates 目录直接运行。
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UPDATE_INFO="$SCRIPT_DIR/UPDATE_INFO"

usage() {
    cat <<'EOF'
用法：sudo bash install_source_update.sh

把源码更新安装到新的版本目录，并将源码符号链接切换到新版本。
旧源码目录和其中的板端修改会保留。本脚本不自动编译或部署程序。
EOF
}

case "${1:-}" in
    -h|--help)
        usage
        exit 0
        ;;
    "") ;;
    *)
        echo "[错误] 未知参数: $1" >&2
        usage >&2
        exit 2
        ;;
esac

[ -f "$UPDATE_INFO" ] && [ -f "$SCRIPT_DIR/SHA256SUMS" ] \
    || { echo "[错误] 安装脚本必须与 UPDATE_INFO、SHA256SUMS 一起使用。" >&2; exit 1; }
for command_name in dpkg dpkg-query sha256sum; do
    command -v "$command_name" >/dev/null 2>&1 \
        || { echo "[错误] 设备缺少安装命令: $command_name" >&2; exit 1; }
done

metadata_value() {
    local key="$1"
    sed -n "s/^${key}=//p" "$UPDATE_INFO" | head -n 1
}

[ "$(metadata_value update_format)" = 1 ] \
    || { echo "[错误] 无法识别源码更新包格式。" >&2; exit 1; }

EXPECTED_ARCH="$(metadata_value deb_arch)"
CURRENT_ARCH="$(dpkg --print-architecture)"
[ "$CURRENT_ARCH" = "$EXPECTED_ARCH" ] \
    || { echo "[错误] deb 架构不匹配：更新包=$EXPECTED_ARCH，设备=$CURRENT_ARCH" >&2; exit 1; }

PACKAGE_NAME="$(metadata_value package_name)"
PACKAGE_VERSION="$(metadata_value package_version)"
SOURCE_INSTALL_PATH="$(metadata_value source_install_path)"
BUILD_META="$(metadata_value required_build_meta)"
REQUIRED_BUILD_VERSION="$(metadata_value required_build_version)"
SOURCE_DEB="$SCRIPT_DIR/apt/${PACKAGE_NAME}_${PACKAGE_VERSION}_${EXPECTED_ARCH}.deb"
[ -f "$SOURCE_DEB" ] \
    || { echo "[错误] 源码更新包缺少: $SOURCE_DEB" >&2; exit 1; }

if [ "$(id -u)" -ne 0 ]; then
    echo "[错误] 安装源码更新需要 root 权限，请使用 sudo 重新执行。" >&2
    exit 1
fi

INSTALLED_BUILD_VERSION="$(dpkg-query -W -f='${Version}' "$BUILD_META" 2>/dev/null || true)"
if [ -z "$INSTALLED_BUILD_VERSION" ] \
        || ! dpkg --compare-versions "$INSTALLED_BUILD_VERSION" ge "$REQUIRED_BUILD_VERSION"; then
    echo "[错误] 设备没有满足要求的完整离线开发环境。" >&2
    echo "       需要: $BUILD_META >= $REQUIRED_BUILD_VERSION" >&2
    echo "       当前: ${INSTALLED_BUILD_VERSION:-未安装}" >&2
    echo "       请先安装与更新包配套或更新的 full-bundle。" >&2
    exit 1
fi

INSTALLED_SOURCE_VERSION="$(dpkg-query -W -f='${Version}' "$PACKAGE_NAME" 2>/dev/null || true)"
if [ -n "$INSTALLED_SOURCE_VERSION" ] \
        && dpkg --compare-versions "$INSTALLED_SOURCE_VERSION" gt "$PACKAGE_VERSION"; then
    echo "[错误] 设备上的源码版本比这个更新包更新，拒绝降级。" >&2
    echo "       设备版本: $INSTALLED_SOURCE_VERSION" >&2
    echo "       更新包版本: $PACKAGE_VERSION" >&2
    exit 1
fi

echo ">>> [1/2] 校验源码更新包..."
(
    cd "$SCRIPT_DIR"
    sha256sum -c SHA256SUMS
)

echo ">>> [2/2] 安装源码更新（保留旧版本目录）..."
env DEBIAN_FRONTEND=noninteractive dpkg -i "$SOURCE_DEB"

echo
echo "[OK] 源码更新完成。"
echo "     新版源码: $SOURCE_INSTALL_PATH"
if [ -L /userdata/rk3588_visual_analysis_framework ]; then
    echo "     固定入口: /userdata/rk3588_visual_analysis_framework"
fi
echo "     本次没有自动编译或覆盖正在运行的程序。"
echo "     主程序编译和安装："
echo "       cd $SOURCE_INSTALL_PATH"
echo "       ./vision package projects/person_count"
echo "       sudo ./install_app.sh dist"
echo "     首次网络配置工具编译："
echo "       cd $SOURCE_INSTALL_PATH/tools/first_net_config && ./build.sh"
echo "     Web 控制台源码有修改时，重新构建并部署："
echo "       cd $SOURCE_INSTALL_PATH/web_console/frontend && npm run build"
echo "       cd $SOURCE_INSTALL_PATH && sudo ./setup/install.sh upgrade offline"
