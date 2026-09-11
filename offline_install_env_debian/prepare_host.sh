#!/usr/bin/env bash
# 在联网 Debian RK3588 制作机上准备项目构建和离线仓库制作工具。
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

[ "$#" -eq 0 ] || {
    echo "[错误] prepare_host.sh 无需命令行参数。" >&2
    exit 2
}

# shellcheck source=/etc/os-release
source /etc/os-release
if [ "${ID:-unknown}" != debian ]; then
    echo "[错误] 本脚本只用于 Debian；当前系统为 ${ID:-unknown} ${VERSION_ID:-unknown}。" >&2
    exit 1
fi
if [ "$(uname -m)" != aarch64 ] || [ "$(dpkg --print-architecture)" != arm64 ]; then
    echo "[错误] 必须在 Debian ARM64 制作机上运行。" >&2
    echo "       当前 uname=$(uname -m), dpkg=$(dpkg --print-architecture)" >&2
    exit 1
fi

# 仓库可能携带旧系统上编译的调试二进制；它不代表当前 Debian 环境缺包。
# 制包器下一步会在当前 Debian 上重新编译，并以新 ELF 识别真实依赖。
# install_deps.sh 会安装运行、编译、制包、Python、Node.js 和前端依赖。
bash "$PROJECT_ROOT/install_deps.sh" --skip-app-check

# 两个制包工具已经列入 Debian APT_BUILD。这里只验收，不再无条件执行
# apt-get install，防止 APT 根据陈旧索引升级本来已经可用的工具。
command -v dpkg-scanpackages >/dev/null 2>&1 \
    || { echo "[错误] install_deps.sh 完成后仍缺少 dpkg-scanpackages。" >&2; exit 1; }
command -v dpkg-repack >/dev/null 2>&1 \
    || { echo "[错误] install_deps.sh 完成后仍缺少 dpkg-repack。" >&2; exit 1; }

echo
echo "[OK] Debian 制作机准备完成。"
if [ "${OFFLINE_PREPARE_EMBEDDED:-false}" = true ]; then
    echo "     返回 create_bundle.sh 继续生成离线包。"
else
    echo "     可执行: bash offline_install_env_debian/create_bundle.sh"
fi
