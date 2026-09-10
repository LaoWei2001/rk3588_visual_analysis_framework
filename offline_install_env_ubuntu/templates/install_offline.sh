#!/usr/bin/env bash
# Ubuntu 制包器复制到 bundle 根目录的独立离线安装器模板；
# 不要从 templates 目录直接运行。
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUNDLE_DIR="$SCRIPT_DIR"

if [ ! -f "$BUNDLE_DIR/BUNDLE_INFO" ] || [ ! -f "$BUNDLE_DIR/apt/Packages" ]; then
    echo "[错误] install_offline.sh 必须位于生成包根目录，并与 BUNDLE_INFO、apt/ 一起使用。" >&2
    exit 1
fi

while [ "$#" -gt 0 ]; do
    case "$1" in
        -h|--help)
            echo "用法：sudo bash install_offline.sh"
            echo "安装器会自动读取离线包类型，无需再次选择完整或精简模式。"
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

EXPECTED_OS_ID="$(metadata_value os_id)"
EXPECTED_OS_VERSION="$(metadata_value os_version_id)"
if [ "$EXPECTED_OS_ID" != ubuntu ]; then
    echo "[错误] 这不是 Ubuntu 离线包：BUNDLE_INFO 中 os_id=${EXPECTED_OS_ID:-未设置}。" >&2
    exit 1
fi
EXPECTED_OS="$EXPECTED_OS_ID $EXPECTED_OS_VERSION"
STRICT_TARGET_OS="$(metadata_value strict_target_os)"
# shellcheck source=/etc/os-release
source /etc/os-release
CURRENT_OS="${ID:-unknown} ${VERSION_ID:-unknown}"
if [ "${ID:-unknown}" != ubuntu ]; then
    echo "[错误] Ubuntu 离线安装器不能安装到 ${ID:-unknown} ${VERSION_ID:-unknown}。" >&2
    exit 1
fi
if [ "$CURRENT_OS" != "$EXPECTED_OS" ]; then
    if [ "$STRICT_TARGET_OS" = true ]; then
        echo "[错误] 系统版本不匹配：离线包=$EXPECTED_OS，当前设备=$CURRENT_OS。" >&2
        echo "       请使用与目标系统版本一致的离线包。" >&2
        exit 1
    fi
    echo "[警告] 制作机系统为 $EXPECTED_OS，当前设备为 $CURRENT_OS。" >&2
    echo "       将继续交给 APT 判断各 deb 是否兼容。" >&2
fi

BUNDLE_PROFILE="$(metadata_value profile)"
case "$BUNDLE_PROFILE" in
    runtime-build)
        WANT_BUILD=true
        echo ">>> 离线包类型: 完整开发包（运行环境、Web 控制台、源码、C/C++ 和前端工具链）"
        ;;
    runtime)
        WANT_BUILD=false
        echo ">>> 离线包类型: 精简环境包（Web 控制台，不含源码和编译工具）"
        ;;
    *)
        echo "[错误] 无法识别离线包类型: ${BUNDLE_PROFILE:-未设置}" >&2
        echo "       请在开发机重新运行 create_bundle.sh。" >&2
        exit 1
        ;;
esac

RUNTIME_META="$(metadata_value runtime_meta_package)"
BUILD_META="$(metadata_value build_meta_package)"
PYTHON_PACKAGE="$(metadata_value python_package)"
ROCKCHIP_PACKAGE="$(metadata_value rockchip_files_package)"
CONSOLE_PACKAGE="$(metadata_value console_package)"
SOURCE_PACKAGE="$(metadata_value source_package)"
SOURCE_INSTALL_PATH="$(metadata_value source_install_path)"
NODE_TOOLCHAIN_PACKAGE="$(metadata_value node_toolchain_package)"
NODE_TOOLCHAIN_ROOT="$(metadata_value node_toolchain_root)"
if [ -z "$CONSOLE_PACKAGE" ]; then
    echo "[错误] 当前离线仓库不包含 Web 控制台 deb，请在开发机重新运行 create_bundle.sh。" >&2
    exit 1
fi
TARGET_PACKAGES=("$CONSOLE_PACKAGE" "$RUNTIME_META")
[ -z "$PYTHON_PACKAGE" ] || TARGET_PACKAGES+=("$PYTHON_PACKAGE")
[ -z "$ROCKCHIP_PACKAGE" ] || TARGET_PACKAGES+=("$ROCKCHIP_PACKAGE")
if [ "$WANT_BUILD" = true ]; then
    if [ -z "$BUILD_META" ]; then
        echo "[错误] 当前仓库没有编译环境，请重新运行默认的 create_bundle.sh。" >&2
        exit 1
    fi
    if [ -z "$SOURCE_PACKAGE" ] || [ -z "$SOURCE_INSTALL_PATH" ]; then
        echo "[错误] 当前仓库没有项目源码包，请在开发机重新运行 create_bundle.sh。" >&2
        exit 1
    fi
    if [ -z "$NODE_TOOLCHAIN_PACKAGE" ] || [ -z "$NODE_TOOLCHAIN_ROOT" ]; then
        echo "[错误] 当前仓库没有 Node.js/npm 前端工具链，请重新运行默认的 create_bundle.sh。" >&2
        exit 1
    fi
    TARGET_PACKAGES+=("$BUILD_META")
    TARGET_PACKAGES+=("$SOURCE_PACKAGE")
    TARGET_PACKAGES+=("$NODE_TOOLCHAIN_PACKAGE")
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

echo ">>> [2/2] 安装项目环境、Web 控制台及其全部依赖..."
if ! as_root env DEBIAN_FRONTEND=noninteractive apt-get "${APT_OPTIONS[@]}" \
    -o Dpkg::Options::=--force-confold \
    --no-install-recommends --reinstall -y install "${TARGET_PACKAGES[@]}"; then
    echo >&2
    echo "[错误] APT 无法用当前离线仓库解决这台设备的软件包关系。" >&2
    if [ "$WANT_BUILD" = false ]; then
        echo "       当前使用的是精简运行包，它不携带 C/C++ 开发包及其依赖。" >&2
        echo "       如果上方错误涉及 *-dev、编译器、CMake 或缺少 libopencv-dev，" >&2
        echo "       请改用开发机生成的 full-bundle；不要在设备上联网补装。" >&2
        echo "       这也适用于新设备、曾安装过编译环境或软件包状态不确定的设备。" >&2
    else
        echo "       请检查是否混用了其他发行版或版本的软件源、是否锁定了软件包，" >&2
        echo "       以及 dpkg --audit 是否报告未配置完成的软件包。" >&2
    fi
    exit 1
fi

echo ">>> 验证系统 Python 环境..."
if ! /usr/bin/python3 - <<'PY'
import importlib
import sys

modules = (
    "fastapi", "starlette", "uvicorn", "pydantic", "aiofiles", "multipart",
    "uvloop", "httptools", "watchfiles", "dotenv", "cv2", "pam", "six", "cffi",
    "yaml", "requests", "websockets",
)
errors = []
for name in modules:
    try:
        importlib.import_module(name)
    except Exception as exc:
        errors.append(f"{name}: {exc}")
if errors:
    print("\n".join(errors), file=sys.stderr)
    raise SystemExit(1)
PY
then
    echo "[错误] 系统 Python 的项目模块导入失败。" >&2
    exit 1
fi
/usr/bin/python3 -m pip check

if [ "$WANT_BUILD" = true ]; then
    echo ">>> 验证 C/C++ 和前端构建环境..."
    for build_command in cmake make gcc g++ pkg-config; do
        command -v "$build_command" >/dev/null 2>&1 \
            || { echo "[错误] 完整包安装后缺少编译命令: $build_command" >&2; exit 1; }
    done
    for pkg_module in \
            librga gtk+-3.0 gstreamer-1.0 gstreamer-video-1.0 \
            gstreamer-allocators-1.0 gstreamer-rtsp-server-1.0; do
        if ! pkg-config --exists "$pkg_module"; then
            echo "[错误] 完整包安装后 pkg-config 无法解析: $pkg_module" >&2
            pkg-config --print-errors --exists "$pkg_module" 2>&1 \
                | sed 's/^/       /' >&2 || true
            exit 1
        fi
    done
    [ -x "$NODE_TOOLCHAIN_ROOT/bin/node" ] && [ -x "$NODE_TOOLCHAIN_ROOT/bin/npm" ] \
        || { echo "[错误] 离线 Node.js/npm 工具链不完整: $NODE_TOOLCHAIN_ROOT" >&2; exit 1; }
    FRONTEND_SOURCE="$SOURCE_INSTALL_PATH/web_console/frontend"
    [ -f "$FRONTEND_SOURCE/package-lock.json" ] && [ -d "$FRONTEND_SOURCE/node_modules" ] \
        || { echo "[错误] 源码包缺少前端源码或 node_modules。" >&2; exit 1; }
    (
        cd "$FRONTEND_SOURCE"
        PATH="$NODE_TOOLCHAIN_ROOT/bin:$PATH" "$NODE_TOOLCHAIN_ROOT/bin/npm" run build
    )
    [ -f "$FRONTEND_SOURCE/dist/index.html" ] \
        || { echo "[错误] 安装后的前端构建验证没有生成 dist/index.html。" >&2; exit 1; }
fi

echo
echo "[OK] RK3588 视觉分析环境和 Web 控制台安装完成。"
echo "     Web 控制台: http://设备IP:8080"
echo "     控制台服务: systemctl status rk3588-console"
echo "     程序目录: /opt/ai_apps（默认不预装任何程序）"
echo "     Python 环境: /usr/bin/python3（系统环境；已满足的包会跳过）"
echo "     RKNN 用户态库: /opt/vision-analysis/rockchip/lib"
if [ "$WANT_BUILD" = true ]; then
    echo "     项目源码: $SOURCE_INSTALL_PATH"
    if [ -L /userdata/rk3588_visual_analysis_framework ]; then
        echo "     源码入口: /userdata/rk3588_visual_analysis_framework"
    fi
    echo "     编译方法: cd $SOURCE_INSTALL_PATH/vision_analysis && ./build.sh dist"
    echo "     Node.js/npm: $NODE_TOOLCHAIN_ROOT/bin"
    echo "     前端构建: cd $SOURCE_INSTALL_PATH/web_console/frontend && PATH=$NODE_TOOLCHAIN_ROOT/bin:\$PATH $NODE_TOOLCHAIN_ROOT/bin/npm run build"
fi
