#!/usr/bin/env bash
# Ubuntu ARM64 离线包入口；实际制包流程与 Debian 共用，依赖清单和输出相互隔离。
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
COMMON_BUILDER="$PROJECT_ROOT/offline_install_env_debian/create_bundle.sh"

[ -f "$COMMON_BUILDER" ] || {
    echo "[错误] 缺少共用制包器: $COMMON_BUILDER" >&2
    echo "       请复制或克隆整个项目，不能只复制 offline_install_env_ubuntu。" >&2
    exit 1
}

# shellcheck source=/etc/os-release
source /etc/os-release
if [ "${ID:-unknown}" != ubuntu ]; then
    echo "[错误] Ubuntu 离线包入口不能在 ${ID:-unknown} ${VERSION_ID:-unknown} 上运行。" >&2
    echo "       请把整个项目复制到联网 Ubuntu RK3588 后再执行。" >&2
    exit 1
fi

export OFFLINE_ENV_DIR="$SCRIPT_DIR"
export OFFLINE_PROJECT_ROOT="$PROJECT_ROOT"
export OFFLINE_EXPECTED_OS_ID=ubuntu
export OFFLINE_ENV_DISPLAY_PATH=offline_install_env_ubuntu
export OFFLINE_STRICT_TARGET_OS=true

exec bash "$COMMON_BUILDER" "$@"
