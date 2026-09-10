#!/usr/bin/env bash
# 在联网 Ubuntu RK3588 制作机上准备项目构建和离线仓库制作工具。
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# shellcheck source=/etc/os-release
source /etc/os-release
if [ "${ID:-unknown}" != ubuntu ]; then
    echo "[错误] 本脚本只用于 Ubuntu；当前系统为 ${ID:-unknown} ${VERSION_ID:-unknown}。" >&2
    exit 1
fi
if [ "$(uname -m)" != aarch64 ] || [ "$(dpkg --print-architecture)" != arm64 ]; then
    echo "[错误] 必须在 Ubuntu ARM64 制作机上运行。" >&2
    echo "       当前 uname=$(uname -m), dpkg=$(dpkg --print-architecture)" >&2
    exit 1
fi

if [ "$(id -u)" -eq 0 ]; then
    ROOT=()
elif command -v sudo >/dev/null 2>&1; then
    ROOT=(sudo)
else
    echo "[错误] 准备制作机需要 root 或 sudo。" >&2
    exit 1
fi

"${ROOT[@]}" apt-get update
"${ROOT[@]}" env DEBIAN_FRONTEND=noninteractive apt-get install -y \
    dpkg-dev dpkg-repack

bash "$PROJECT_ROOT/install_deps.sh"

echo
echo "[OK] Ubuntu 制作机准备完成。"
echo "     下一步: bash offline_install_env_ubuntu/create_bundle.sh --refresh-debs"
