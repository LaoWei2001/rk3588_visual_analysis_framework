#!/usr/bin/env bash
# ============================================================================
# RK3588 第三方环境一键配置（Debian / Ubuntu / Armbian）
#
#   bash install_deps.sh              安装全部运行环境并预构建 Web 前端
#   bash install_deps.sh --build      再安装板端 C/C++ 编译与开发工具
#   bash install_deps.sh --check      不联网、不修改系统，只验收运行环境
#   bash install_deps.sh --check --build   连同编译环境一起验收
#
# 正常安装必须在盒子仍能访问 APT、PyPI/npm 镜像时执行。安装成功后，现场运行、
# 摄像头管理、视频预览/录像和已构建的 Web 控制台均不再依赖公网；--check 可在
# 断网现场重复执行。它不是自带 deb/wheel/npm 包的“离线安装包”。
#
# 不在本脚本职责内：Rockchip BSP 自带的 RKNN、RGA、MPP 及其 GStreamer 插件。
# ============================================================================
set -Eeuo pipefail

WANT_BUILD=false
CHECK_ONLY=false

usage() {
    sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build) WANT_BUILD=true ;;
        --check) CHECK_ONLY=true ;;
        -h|--help) usage; exit 0 ;;
        *) echo "[错误] 未知参数: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FRONTEND_DIR="$PROJ/web_console/frontend"
NODE_VERSION="${NODE_VERSION:-v20.18.0}"
NODE_TEMP_DIR=""

if [[ ! "$NODE_VERSION" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "[错误] NODE_VERSION 必须采用 v主版本.次版本.补丁版本 格式" >&2
    exit 2
fi

cleanup() {
    if [ -n "$NODE_TEMP_DIR" ] && [ -d "$NODE_TEMP_DIR" ]; then
        rm -rf -- "$NODE_TEMP_DIR"
    fi
}
trap cleanup EXIT

if [ "$(id -u)" -eq 0 ]; then
    ROOT=()
elif command -v sudo >/dev/null 2>&1; then
    ROOT=(sudo)
else
    echo "[错误] 安装系统包需要 root 或 sudo；可用 --check 只做检查。" >&2
    [ "$CHECK_ONLY" = true ] || exit 1
    ROOT=()
fi

as_root() {
    "${ROOT[@]}" "$@"
}

echo "============================================================"
echo "  RK3588 第三方环境配置  (项目根: $PROJ)"
echo "  模式: $([ "$CHECK_ONLY" = true ] && echo '只检查' || echo '安装并检查')$([ "$WANT_BUILD" = true ] && echo ' + C/C++ 编译环境' || true)"
echo "  Rockchip RKNN/RGA/MPP: 使用厂家系统，不由本脚本安装"
echo "============================================================"

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

install_apt_dependencies() {
    echo ">>> [1/5] 安装 APT 第三方依赖..."
    if ! as_root apt-get update; then
        echo "    [警告] apt-get update 有源失败；继续使用现有缓存尝试安装。" >&2
    fi
    as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y "${APT_RUNTIME[@]}"
    if [ "$WANT_BUILD" = true ]; then
        as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y "${APT_BUILD[@]}"
    fi
}

node_major_is_supported() {
    command -v node >/dev/null 2>&1 || return 1
    local major
    major="$(node -v 2>/dev/null | sed -n 's/^v\([0-9][0-9]*\).*/\1/p')"
    [ -n "$major" ] && [ "$major" -ge 18 ]
}

install_node() {
    echo ">>> [2/5] 准备 Node.js 18+ 与 npm..."
    if node_major_is_supported && command -v npm >/dev/null 2>&1; then
        echo "    已有 Node $(node -v) / npm $(npm -v)"
        return
    fi

    local node_arch
    case "$(uname -m)" in
        aarch64|arm64) node_arch="arm64" ;;
        x86_64|amd64) node_arch="x64" ;;
        *) echo "[错误] Node 预编译包不支持当前架构 $(uname -m)" >&2; exit 1 ;;
    esac

    local package="node-${NODE_VERSION}-linux-${node_arch}"
    NODE_TEMP_DIR="$(mktemp -d)"
    local archive="$NODE_TEMP_DIR/${package}.tar.xz"
    local checksums="$NODE_TEMP_DIR/SHASUMS256.txt"
    local verified=false
    local base
    for base in \
        "https://registry.npmmirror.com/-/binary/node" \
        "https://mirrors.tuna.tsinghua.edu.cn/nodejs-release" \
        "https://nodejs.org/dist"; do
        echo "    下载并校验: $base/$NODE_VERSION/${package}.tar.xz"
        if curl -fSL --connect-timeout 15 --retry 2 -o "$archive" \
                "$base/$NODE_VERSION/${package}.tar.xz" \
            && curl -fSL --connect-timeout 15 --retry 2 -o "$checksums" \
                "$base/$NODE_VERSION/SHASUMS256.txt" \
            && grep " ${package}\.tar\.xz$" "$checksums" > "$NODE_TEMP_DIR/package.sha256" \
            && (cd "$NODE_TEMP_DIR" && sha256sum -c package.sha256); then
            verified=true
            break
        fi
        echo "    该镜像下载或校验失败，尝试下一个..."
    done
    if [ "$verified" != true ]; then
        echo "[错误] Node.js 下载失败；没有校验通过的安装包，停止配置。" >&2
        exit 1
    fi

    local install_parent="/usr/local/lib/nodejs"
    local install_root="$install_parent/$package"
    as_root mkdir -p "$install_parent" /usr/local/bin
    as_root tar -xJf "$archive" -C "$install_parent"
    local executable
    for executable in node npm npx corepack; do
        if [ -e "$install_root/bin/$executable" ]; then
            as_root ln -sfn "$install_root/bin/$executable" "/usr/local/bin/$executable"
        fi
    done
    hash -r 2>/dev/null || true
    node_major_is_supported && command -v npm >/dev/null 2>&1 \
        || { echo "[错误] Node.js 安装后仍不可用" >&2; exit 1; }
    echo "    已安装 Node $(node -v) / npm $(npm -v)"
}

install_python_dependencies() {
    echo ">>> [3/5] 安装全部 Python requirements..."
    local pip_system_args=()
    local pip_install_help
    pip_install_help="$(python3 -m pip help install 2>/dev/null || true)"
    if grep -q -- '--break-system-packages' <<< "$pip_install_help"; then
        pip_system_args+=(--break-system-packages)
    fi
    as_root python3 -m pip install "${pip_system_args[@]}" --upgrade pip setuptools wheel

    local requirements=()
    while IFS= read -r -d '' requirement; do
        requirements+=("$requirement")
    done < <(
        find "$PROJ" \
            \( -path '*/node_modules' -o -path '*/tests' -o -path '*/build' -o -path '*/dist' \) -prune \
            -o -type f -name requirements.txt -print0 | sort -z
    )
    [ "${#requirements[@]}" -gt 0 ] || { echo "[错误] 未找到 requirements.txt" >&2; exit 1; }

    local pip_args=()
    local requirement
    for requirement in "${requirements[@]}"; do
        echo "    -> ${requirement#$PROJ/}"
        pip_args+=(-r "$requirement")
    done
    # 一次性交给解析器，避免逐文件安装把另一个组件的固定版本静默覆盖。
    as_root python3 -m pip install "${pip_system_args[@]}" --prefer-binary "${pip_args[@]}"
}

prepare_frontend() {
    echo ">>> [4/5] 锁定安装并预构建 Web 前端..."
    [ -f "$FRONTEND_DIR/package-lock.json" ] \
        || { echo "[错误] 缺少 $FRONTEND_DIR/package-lock.json" >&2; exit 1; }
    (
        cd "$FRONTEND_DIR"
        npm ci --no-audit --no-fund
        npm run build
    )
    [ -f "$FRONTEND_DIR/dist/index.html" ] \
        || { echo "[错误] Web 前端构建未生成 dist/index.html" >&2; exit 1; }
}

CHECK_ERRORS=0

check_fail() {
    echo "    [缺失] $*" >&2
    CHECK_ERRORS=$((CHECK_ERRORS + 1))
}

check_environment() {
    echo ">>> [5/5] 验收第三方运行环境..."

    local runtime_commands=(
        bash curl python3 nmcli nm-online ip ping ethtool
        systemctl systemd-run journalctl timedatectl pgrep xhost
        gst-launch-1.0 gst-inspect-1.0 ffprobe v4l2-ctl
        node npm
    )
    local command_name
    for command_name in "${runtime_commands[@]}"; do
        command -v "$command_name" >/dev/null 2>&1 || check_fail "系统命令 $command_name"
    done

    [ -d /run/systemd/system ] || check_fail "systemd 未作为当前系统服务管理器运行"
    if command -v nmcli >/dev/null 2>&1; then
        nmcli -t general status >/dev/null 2>&1 \
            || check_fail "NetworkManager 服务不可用"
    fi
    if ! command -v Xorg >/dev/null 2>&1 && ! command -v X >/dev/null 2>&1; then
        echo "    [提示] 未检测到 X Server；不影响无界面部署模式，但 HDMI 调试显示不可用。"
    fi

    if node_major_is_supported; then
        echo "    Node: $(node -v)"
    else
        check_fail "Node.js 版本必须 >= 18"
    fi

    if ! python3 - <<'PY'
import importlib
import sys

modules = (
    "fastapi", "starlette", "uvicorn", "pydantic", "aiofiles", "multipart",
    "uvloop", "httptools", "watchfiles", "dotenv",
    "cv2", "pam", "yaml", "requests", "websockets",
)
errors = []
for name in modules:
    try:
        importlib.import_module(name)
    except Exception as exc:
        errors.append(f"{name}: {exc}")
if errors:
    print("    [缺失] Python 模块导入失败：", file=sys.stderr)
    for error in errors:
        print(f"      - {error}", file=sys.stderr)
    raise SystemExit(1)
print(f"    Python: {sys.version.split()[0]}，核心模块导入正常")
PY
    then
        CHECK_ERRORS=$((CHECK_ERRORS + 1))
    fi
    python3 -m pip check || CHECK_ERRORS=$((CHECK_ERRORS + 1))

    local gst_elements=(
        appsrc appsink capsfilter filesrc filesink fdsink queue tee decodebin
        rtspsrc rtph264depay rtph265depay rtph264pay rtph265pay
        h264parse h265parse mp4mux avmux_mp4
        v4l2src videorate videoscale videoconvert
        jpegparse jpegdec jpegenc x264enc x265enc
    )
    if command -v gst-inspect-1.0 >/dev/null 2>&1; then
        local element
        for element in "${gst_elements[@]}"; do
            gst-inspect-1.0 "$element" >/dev/null 2>&1 \
                || check_fail "GStreamer element $element"
        done
    fi

    local shared_libraries=(
        'libpam\.so' 'libgpiod\.so' 'libgtk-3\.so'
        'libopencv_freetype\.so' 'libgstrtspserver-1\.0\.so'
    )
    if command -v ldconfig >/dev/null 2>&1; then
        local library
        local ld_cache
        ld_cache="$(ldconfig -p 2>/dev/null || true)"
        for library in "${shared_libraries[@]}"; do
            grep -q "$library" <<< "$ld_cache" \
                || check_fail "共享库 ${library//\\/}"
        done
    else
        check_fail "系统命令 ldconfig"
    fi

    if [ ! -f /usr/share/fonts/truetype/wqy/wqy-zenhei.ttc ] \
        && [ ! -f /usr/share/fonts/truetype/wqy/wqy-microhei.ttc ] \
        && [ ! -f "$PROJ/vision_analysis/assets/fonts/overlay.ttf" ] \
        && [ ! -f "$PROJ/vision_analysis/assets/fonts/overlay.ttc" ] \
        && [ ! -f "$PROJ/vision_analysis/assets/fonts/overlay.otf" ]; then
        check_fail "中文叠字字体（文泉驿或 assets/fonts）"
    fi

    if [ ! -f "$FRONTEND_DIR/dist/index.html" ]; then
        check_fail "预构建前端 web_console/frontend/dist/index.html"
    fi
    if [ ! -d "$FRONTEND_DIR/node_modules" ]; then
        check_fail "前端 node_modules（离线重建缓存）"
    elif command -v npm >/dev/null 2>&1; then
        (cd "$FRONTEND_DIR" && npm ls --depth=0 >/dev/null) \
            || check_fail "前端 npm 依赖树"
    fi

    if [ "$WANT_BUILD" = true ]; then
        local build_commands=(cmake make gcc g++ pkg-config readelf strip rsync git clang-format)
        for command_name in "${build_commands[@]}"; do
            command -v "$command_name" >/dev/null 2>&1 || check_fail "编译命令 $command_name"
        done
        if command -v cmake >/dev/null 2>&1; then
            local cmake_version
            cmake_version="$(cmake --version | sed -n '1s/.* //p')"
            if ! printf '%s\n%s\n' '3.16.0' "$cmake_version" | sort -V -C; then
                check_fail "CMake >= 3.16（当前 ${cmake_version:-未知}）"
            fi
        fi
        if command -v pkg-config >/dev/null 2>&1; then
            pkg-config --exists gtk+-3.0 gstreamer-1.0 gstreamer-video-1.0 \
                gstreamer-allocators-1.0 gstreamer-rtsp-server-1.0 \
                || check_fail "C/C++ pkg-config 开发模块"
        fi
        [ -f /usr/include/gpiod.h ] || check_fail "libgpiod 开发头文件"
        local freetype_header
        freetype_header="$(find /usr/include /usr/local/include \
            -path '*/opencv2/freetype.hpp' -print -quit 2>/dev/null || true)"
        if [ -z "$freetype_header" ]; then
            check_fail "OpenCV contrib freetype 开发头文件"
        fi
    fi

    local binary
    for binary in \
        "$PROJ/vision_analysis/vision_analysis" \
        "$PROJ/vision_analysis/build/vision_analysis" \
        "$PROJ/first_net_config/first_net_config"; do
        if [ -x "$binary" ]; then
            local missing
            missing="$(ldd "$binary" 2>/dev/null | grep 'not found' \
                | grep -vE 'libr(knnrt|ga)\.so' || true)"
            [ -z "$missing" ] || check_fail "$(basename "$binary") 动态库：$missing"
        fi
    done

    if [ "$CHECK_ERRORS" -ne 0 ]; then
        echo "[失败] 环境验收发现 $CHECK_ERRORS 类缺失。" >&2
        return 1
    fi
    echo "    系统命令、Python、GStreamer、共享库和前端构建均通过。"
}

if [ "$CHECK_ONLY" != true ]; then
    install_apt_dependencies
    install_node
    install_python_dependencies
    prepare_frontend
fi

check_environment

echo ""
if [ "$CHECK_ONLY" = true ]; then
    if [ "$WANT_BUILD" = true ]; then
        echo "[OK] 运行环境及 C/C++ 编译环境验收全部通过，无需安装或修改系统。"
    else
        echo "[OK] 运行环境验收全部通过，无需安装或修改系统。"
    fi
    echo "  本次仅执行检查：未联网、未安装软件、未修改系统。"
    echo "  当前设备的第三方环境已就绪，可以直接使用。"
else
    echo "[OK] 第三方环境安装并验收完成。"
    echo "  到达断网现场后，可运行以下命令复检环境："
    echo "    bash install_deps.sh --check$([ "$WANT_BUILD" = true ] && echo ' --build' || true)"
    echo "  仅在需要重新部署 Web 控制台时，才运行："
    echo "    sudo env OFFLINE=1 bash web_console/install.sh"
fi
echo "  说明：Rockchip RKNN/RGA/MPP 由厂家系统提供，不属于本脚本的安装和验收范围。"
