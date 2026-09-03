#!/usr/bin/env bash
# ============================================================================
# RK3588 第三方环境一键配置（Debian / Ubuntu / Armbian）
#
#   bash install_deps.sh              安装全部运行环境并预构建 Web 前端
#   bash install_deps.sh --build      再安装板端 C/C++ 编译与开发工具
#   bash install_deps.sh --check      不联网、不修改系统，只验收运行环境
#   bash install_deps.sh --check --build   连同编译环境一起验收
#
# 正常安装必须在盒子仍能访问 APT、PyPI/npm 镜像时执行。断网设备请先在同版本
# 有网 Debian RK3588 上运行 offline_install_env_debian/create_bundle.sh；现场直接运行统一安装入口。
# 安装成功后，--check 可在断网现场重复执行。
#
# 不在本脚本职责内：Rockchip BSP 的 RKNPU 内核驱动、RGA、MPP 及其 GStreamer 插件。
# 用户态 librknnrt.so 已由 vision_analysis/vendor/rknn/ 固定并随应用包发布。
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
DEPENDENCY_MANIFEST="$PROJ/offline_install_env_debian/dependency_manifest.sh"
if [ ! -f "$DEPENDENCY_MANIFEST" ]; then
    echo "[错误] 缺少依赖清单: $DEPENDENCY_MANIFEST" >&2
    exit 1
fi
# shellcheck source=offline_install_env_debian/dependency_manifest.sh
source "$DEPENDENCY_MANIFEST"
NODE_VERSION="${NODE_VERSION:-$DEFAULT_NODE_VERSION}"
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
echo "  Rockchip RKNPU驱动/RGA/MPP: 使用厂家系统；librknnrt.so 由应用包固定"
echo "============================================================"

apt_package_is_installed() {
    # ${Status} 的首字段可能是 hold；只检查实际安装状态，避免把 hold 包误判为缺失。
    dpkg-query -W -f='${db:Status-Status}\n' "$1" 2>/dev/null \
        | grep -qx 'installed'
}

install_missing_apt_packages() {
    local group_name="$1"
    shift

    local missing_packages=()
    local package
    for package in "$@"; do
        if ! apt_package_is_installed "$package"; then
            missing_packages+=("$package")
        fi
    done

    if [ "${#missing_packages[@]}" -eq 0 ]; then
        echo "    ${group_name}已全部安装；保留当前版本。"
        return
    fi

    echo "    ${group_name}缺失 ${#missing_packages[@]} 个包：${missing_packages[*]}"
    # Rockchip BSP 经常 hold 多媒体相关包。只补装缺失项，并禁止 APT 顺带升级
    # 命令行中已经安装的包，避免 -y 因尝试改变 hold 包而中止整个事务。
    if ! as_root env DEBIAN_FRONTEND=noninteractive \
            apt-get install -y --no-upgrade "${missing_packages[@]}"; then
        echo "[错误] ${group_name}安装失败。脚本不会自动解除 hold 或升级厂家 BSP 包。" >&2
        echo "       请检查上方 APT 输出、软件源及依赖版本冲突。" >&2
        return 1
    fi
}

install_apt_dependencies() {
    echo ">>> [1/5] 安装 APT 第三方依赖..."
    if ! as_root apt-get update; then
        echo "    [警告] apt-get update 有软件源失败；继续使用已成功更新的索引和现有缓存。" >&2
    fi
    install_missing_apt_packages "运行环境" "${APT_RUNTIME[@]}"
    if [ "$WANT_BUILD" = true ]; then
        install_missing_apt_packages "C/C++ 编译环境" "${APT_BUILD[@]}"
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

    local pip_args=()
    local requirement_relative
    local requirement
    for requirement_relative in "${PYTHON_REQUIREMENTS[@]}"; do
        requirement="$PROJ/$requirement_relative"
        [ -f "$requirement" ] \
            || { echo "[错误] 依赖清单中的文件不存在: $requirement_relative" >&2; exit 1; }
        echo "    -> $requirement_relative"
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
CHECK_WARNINGS=0
CHECK_PASSES=0

check_fail() {
    echo "    [失败] $*" >&2
    CHECK_ERRORS=$((CHECK_ERRORS + 1))
}

check_warn() {
    echo "    [警告] $*" >&2
    CHECK_WARNINGS=$((CHECK_WARNINGS + 1))
}

check_pass() {
    echo "    [通过] $*"
    CHECK_PASSES=$((CHECK_PASSES + 1))
}

check_info() {
    echo "    [信息] $*"
}

manifest_value() {
    local manifest="$1"
    local key="$2"
    awk -F= -v wanted="$key" '$1 == wanted {sub(/^[^=]*=/, ""); print; exit}' "$manifest"
}

check_device_access() {
    local label="$1"
    local path="$2"
    local group_hint="$3"
    if [ ! -e "$path" ]; then
        check_fail "$label 不存在: $path；请检查厂家 BSP/内核驱动。"
        return
    fi
    if [ ! -r "$path" ] || [ ! -w "$path" ]; then
        check_warn "$label 已存在但当前用户 $(id -un) 无读写权限: $path；应用以非 root 运行时请加入 $group_hint 组或配置 udev 权限。"
    else
        check_pass "$label 可由当前用户读写: $path。"
    fi
}

check_platform_environment() {
    local os_id="unknown"
    local os_version="unknown"
    local os_pretty="unknown"
    local machine_arch
    local model="unknown"
    local compatible=""
    local kernel_release
    local kernel_numeric
    local glibc_version="unknown"
    local compat_manifest="$PROJ/vision_analysis/vendor/rockchip/PLATFORM_COMPATIBILITY.env"
    local expected_os_id=""
    local expected_os_version=""
    local expected_kernel=""

    if [ -r /etc/os-release ]; then
        os_id="$(. /etc/os-release; printf '%s' "${ID:-unknown}")"
        os_version="$(. /etc/os-release; printf '%s' "${VERSION_ID:-unknown}")"
        os_pretty="$(. /etc/os-release; printf '%s' "${PRETTY_NAME:-unknown}")"
    fi
    machine_arch="$(uname -m)"
    kernel_release="$(uname -r)"
    kernel_numeric="${kernel_release%%-*}"
    if command -v getconf >/dev/null 2>&1; then
        glibc_version="$(getconf GNU_LIBC_VERSION 2>/dev/null || printf unknown)"
    fi
    if [ -r /proc/device-tree/model ]; then
        model="$(tr -d '\000' < /proc/device-tree/model)"
    fi
    if [ -r /proc/device-tree/compatible ]; then
        compatible="$(tr '\000' ',' < /proc/device-tree/compatible)"
    fi

    check_info "系统: $os_pretty；内核: $kernel_release；libc: $glibc_version"
    check_info "硬件: $model；架构: $machine_arch"
    case "$machine_arch" in
        aarch64|arm64) check_pass "CPU 架构为 ARM64。" ;;
        *) check_fail "CPU 架构是 $machine_arch，本项目运行产物要求 AArch64/RK3588。" ;;
    esac
    if grep -qi 'rk3588' <<< "$compatible,$model"; then
        check_pass "设备树识别为 RK3588。"
    else
        check_fail "设备树未识别到 RK3588；compatible=${compatible:-不可读}。"
    fi

    if [ -r "$compat_manifest" ]; then
        expected_os_id="$(manifest_value "$compat_manifest" validated_os_id)"
        expected_os_version="$(manifest_value "$compat_manifest" validated_os_version_id)"
        expected_kernel="$(manifest_value "$compat_manifest" validated_kernel)"
        if [ "$os_id" = "$expected_os_id" ] && [ "$os_version" = "$expected_os_version" ]; then
            check_pass "发行版符合已验证基线: $expected_os_id $expected_os_version。"
        else
            check_warn "当前发行版是 $os_id $os_version，已验证基线是 $expected_os_id $expected_os_version；必须重新验证系统动态库及硬件链路。"
        fi
        if [ "$kernel_numeric" = "$expected_kernel" ]; then
            check_pass "内核版本符合已验证基线: $expected_kernel。"
        else
            check_warn "当前内核 $kernel_numeric 与已验证基线 $expected_kernel 不同；重点复测 RKNPU/RGA/MPP。"
        fi
    else
        check_warn "缺少平台兼容性基线: $compat_manifest；无法判断当前 BSP 是否为已验证组合。"
    fi

    local available_kb="0"
    available_kb="$(df -Pk "$PROJ" 2>/dev/null | awk 'NR == 2 {print $4}')"
    if [[ "$available_kb" =~ ^[0-9]+$ ]]; then
        check_info "项目分区可用空间约 $((available_kb / 1024)) MiB。"
        if [ "$available_kb" -lt $((512 * 1024)) ]; then
            check_warn "项目分区可用空间不足 512 MiB；录像、模型更新或日志可能很快写满磁盘。"
        fi
    fi
}

check_rockchip_bsp() {
    local compat_manifest="$PROJ/vision_analysis/vendor/rockchip/PLATFORM_COMPATIBILITY.env"
    local rknn_root="$PROJ/vision_analysis/vendor/rknn/2.4.2a2"
    local rknn_manifest="$rknn_root/RUNTIME_MANIFEST.txt"
    local expected_rknpu=""
    local expected_rga=""
    local actual_rknpu="unknown"
    local actual_rga="unknown"
    local version_line=""
    local render_sys
    local render_driver
    local render_device=""

    if [ -r "$compat_manifest" ]; then
        expected_rknpu="$(manifest_value "$compat_manifest" validated_rknpu_driver)"
        expected_rga="$(manifest_value "$compat_manifest" validated_rga_driver)"
    fi

    if [ -d /sys/module/rknpu ]; then
        check_pass "RKNPU 内核驱动已加载。"
    else
        check_fail "未检测到 /sys/module/rknpu；请安装并加载厂家 RKNPU 驱动。"
    fi
    if [ -r /sys/kernel/debug/rknpu/version ]; then
        version_line="$(tr '\n' ' ' < /sys/kernel/debug/rknpu/version)"
        actual_rknpu="$(sed -n 's/.*\(v[0-9][0-9.]*\).*/\1/p' <<< "$version_line")"
        [ -n "$actual_rknpu" ] || actual_rknpu="unknown"
        if [ -n "$expected_rknpu" ] && [ "$actual_rknpu" = "$expected_rknpu" ]; then
            check_pass "RKNPU 驱动 $actual_rknpu 符合已验证基线。"
        elif [ "$actual_rknpu" != "unknown" ]; then
            check_warn "RKNPU 驱动为 $actual_rknpu，已验证版本为 ${expected_rknpu:-未登记}；不能仅因同为官方版本就视为兼容。"
        fi
    else
        check_warn "RKNPU 驱动已加载，但无法读取 /sys/kernel/debug/rknpu/version；可能未挂载 debugfs，无法核对版本。"
    fi

    for render_sys in /sys/class/drm/renderD*; do
        [ -e "$render_sys" ] || continue
        render_driver="$(basename "$(readlink -f "$render_sys/device/driver" 2>/dev/null)" 2>/dev/null || true)"
        if [[ "${render_driver,,}" = "rknpu" ]]; then
            render_device="/dev/dri/$(basename "$render_sys")"
            break
        fi
    done
    if [ -n "$render_device" ]; then
        check_pass "RKNPU 设备节点已绑定: $render_device。"
        check_device_access "RKNPU 设备" "$render_device" "render"
    else
        check_fail "没有找到由 RKNPU 驱动绑定的 /dev/dri/renderD* 节点。"
    fi

    if [ -f "$rknn_manifest" ] && [ -f "$rknn_root/SHA256SUMS" ] \
            && (cd "$rknn_root" && sha256sum -c --quiet SHA256SUMS); then
        local runtime_version="unknown"
        runtime_version="$(manifest_value "$rknn_manifest" runtime_version)"
        check_pass "项目固定 RKNN Runtime $runtime_version 的头文件和 librknnrt.so 哈希正确。"
    else
        check_fail "项目固定 RKNN Runtime 缺失或哈希错误: $rknn_root。"
    fi

    if [ -d /sys/module/rockchip_rga ]; then
        check_pass "Rockchip RGA 内核驱动已加载。"
    else
        check_fail "未检测到 /sys/module/rockchip_rga；RGA 图像转换不可用。"
    fi
    check_device_access "RGA 设备" /dev/rga "video"
    if [ -r /sys/kernel/debug/rkrga/driver_version ]; then
        version_line="$(tr '\n' ' ' < /sys/kernel/debug/rkrga/driver_version)"
        actual_rga="$(sed -n 's/.*\(v[0-9][0-9.]*\).*/\1/p' <<< "$version_line")"
        [ -n "$actual_rga" ] || actual_rga="unknown"
        if [ -n "$expected_rga" ] && [ "$actual_rga" = "$expected_rga" ]; then
            check_pass "RGA 驱动 $actual_rga 符合已验证基线。"
        elif [ "$actual_rga" != "unknown" ]; then
            check_warn "RGA 驱动为 $actual_rga，已验证版本为 ${expected_rga:-未登记}；需复测颜色转换和 DMA-BUF 零拷贝。"
        fi
    else
        check_warn "RGA 驱动存在，但无法读取 driver_version；无法核对用户态 librga 与驱动组合。"
    fi

    check_device_access "MPP 设备" /dev/mpp_service "video"
    if [ -e /dev/dma_heap/system ]; then
        check_pass "DMA-HEAP 设备存在，可供 RKNPU/RGA/MPP 零拷贝使用。"
        check_device_access "DMA-HEAP system" /dev/dma_heap/system "video"
    else
        check_fail "缺少 /dev/dma_heap/system；零拷贝内存分配可能失败。"
    fi

    local video_count="0"
    local gpio_count="0"
    video_count="$(find /dev -maxdepth 1 -type c -name 'video*' 2>/dev/null | wc -l)"
    gpio_count="$(find /dev -maxdepth 1 -type c -name 'gpiochip*' 2>/dev/null | wc -l)"
    if [ "$video_count" -gt 0 ]; then
        check_info "检测到 $video_count 个 /dev/video* 节点；具体 USB/MIPI 节点仍需按配置实测。"
    else
        check_warn "没有 /dev/video* 节点；RTSP 输入不受影响，但本地摄像头不可用。"
    fi
    if [ "$gpio_count" -gt 0 ]; then
        check_info "检测到 $gpio_count 个 GPIO character device；继电器引脚仍需按板卡定义实测。"
    else
        check_warn "没有 /dev/gpiochip* 节点；GPIO/继电器功能不可用。"
    fi
}

gst_element_description() {
    local element="$1"
    local details
    local version
    local filename
    details="$(gst-inspect-1.0 "$element" 2>/dev/null)" || return 1
    version="$(sed -n 's/^  Version[[:space:]]*//p' <<< "$details" | head -n 1)"
    filename="$(sed -n 's/^  Filename[[:space:]]*//p' <<< "$details" | head -n 1)"
    printf '%s%s' "${version:-版本未知}" "${filename:+，$filename}"
}

check_rockchip_gstreamer() {
    local compat_manifest="$PROJ/vision_analysis/vendor/rockchip/PLATFORM_COMPATIBILITY.env"
    local expected_mpp=""
    local description=""
    local actual_mpp=""
    [ -r "$compat_manifest" ] \
        && expected_mpp="$(manifest_value "$compat_manifest" validated_gst_mpp_version)"

    if description="$(gst_element_description mppvideodec)"; then
        actual_mpp="${description%%，*}"
        check_pass "必需的 Rockchip 硬件解码器 mppvideodec 可用（$description）。"
        if [ -n "$expected_mpp" ] && [ "$actual_mpp" != "$expected_mpp" ]; then
            check_warn "Rockchip MPP 插件版本为 $actual_mpp，已验证版本为 $expected_mpp；需复测 RTSP 长时间解码和断流重连。"
        fi
    else
        check_fail "缺少 mppvideodec；当前 RTSP/文件解码代码无法创建硬件解码器。请安装厂家 rockchipmpp GStreamer 插件。"
    fi

    if description="$(gst_element_description mpph264enc)"; then
        check_pass "Rockchip H.264 硬件编码器 mpph264enc 可用（$description）。"
    elif gst-inspect-1.0 x264enc >/dev/null 2>&1; then
        check_warn "缺少 mpph264enc，将回退 x264enc 软件编码；多路 RTSP 输出时 CPU 占用会明显升高。"
    else
        check_fail "mpph264enc 与 x264enc 都不可用，无法输出 H.264 RTSP。"
    fi
    if description="$(gst_element_description mpph265enc)"; then
        check_pass "Rockchip H.265 硬件编码器 mpph265enc 可用（$description）。"
    elif gst-inspect-1.0 x265enc >/dev/null 2>&1; then
        check_warn "缺少 mpph265enc，将回退 x265enc 软件编码；多路输出性能可能不足。"
    else
        check_fail "mpph265enc 与 x265enc 都不可用，无法输出 H.265 RTSP。"
    fi
    if description="$(gst_element_description mppjpegdec)"; then
        check_pass "Rockchip JPEG 硬件解码器 mppjpegdec 可用（$description）。"
    elif gst-inspect-1.0 jpegdec >/dev/null 2>&1; then
        check_warn "缺少 mppjpegdec，将使用 jpegdec 软件解码。"
    else
        check_fail "mppjpegdec 与 jpegdec 都不可用，JPEG 输入/事件录像处理可能失败。"
    fi
}

check_userland_environment() {
    local errors_before="$CHECK_ERRORS"
    local runtime_commands=(
        bash curl python3 nmcli nm-online ip ping ethtool
        systemctl systemd-run journalctl timedatectl pgrep
        gst-launch-1.0 gst-inspect-1.0 ffprobe v4l2-ctl
        node npm dpkg ldd readelf sha256sum
    )
    local missing_commands=()
    local command_name
    for command_name in "${runtime_commands[@]}"; do
        command -v "$command_name" >/dev/null 2>&1 || missing_commands+=("$command_name")
    done
    if [ "${#missing_commands[@]}" -eq 0 ]; then
        check_pass "项目需要的系统命令均可用。"
    else
        check_fail "缺少系统命令: ${missing_commands[*]}；请重新运行对应系统的依赖安装器。"
    fi

    if [ -d /run/systemd/system ]; then
        check_pass "systemd 正在作为宿主机服务管理器运行。"
    else
        check_fail "systemd 未作为当前服务管理器运行；Web 控制台无法通过 systemd-run 管理视觉进程。"
    fi
    if [ -S /run/dbus/system_bus_socket ]; then
        check_pass "系统 D-Bus 可用。"
    else
        check_fail "缺少 /run/dbus/system_bus_socket；NetworkManager/systemctl 控制可能失败。"
    fi
    if command -v nmcli >/dev/null 2>&1; then
        if nmcli -t general status >/dev/null 2>&1; then
            check_pass "NetworkManager 服务可响应 nmcli。"
        else
            check_fail "NetworkManager 服务不可用；网络配置页面无法工作。可检查: systemctl status NetworkManager"
        fi
    fi
    if ! command -v Xorg >/dev/null 2>&1 && ! command -v X >/dev/null 2>&1; then
        check_info "未检测到 X Server；不影响无界面部署模式，但 HDMI 调试显示不可用。"
    else
        check_info "检测到 X Server；使用 HDMI 显示时还需确认 DISPLAY/Xauthority。"
    fi

    if node_major_is_supported; then
        check_pass "Node.js $(node -v) / npm $(npm -v) 满足前端要求。"
    else
        check_fail "Node.js 版本必须 >= 18；请重新运行联网或离线依赖安装器。"
    fi

    if python3 - <<'PY'
import importlib
import sys

modules = (
    "fastapi", "starlette", "uvicorn", "pydantic", "aiofiles", "multipart",
    "uvloop", "httptools", "watchfiles", "dotenv",
    "cv2", "pam", "six", "yaml", "requests", "websockets",
)
errors = []
for name in modules:
    try:
        importlib.import_module(name)
    except Exception as exc:
        errors.append(f"{name}: {exc}")
if errors:
    for error in errors:
        print(error, file=sys.stderr)
    raise SystemExit(1)
PY
    then
        check_pass "Python $(python3 -c 'import sys; print(sys.version.split()[0])') 核心模块均可导入。"
    else
        check_fail "Python 核心模块导入失败；查看上方模块名，并重新安装 requirements。"
    fi
    if python3 -m pip check; then
        check_pass "pip 依赖关系无冲突。"
    else
        check_fail "pip 报告依赖缺失或版本冲突；断网机请重新运行 offline_install_env_debian/install_offline.sh。"
    fi

    local dpkg_audit_output=""
    dpkg_audit_output="$(dpkg --audit 2>&1 || true)"
    if [ -z "$dpkg_audit_output" ]; then
        check_pass "Debian 软件包数据库没有未完成配置或损坏的软件包。"
    else
        check_warn "Debian 软件包数据库存在异常；可能不立即阻止程序运行，但应执行 dpkg --audit 处理。"
        printf '%s\n' "$dpkg_audit_output" | sed -n '1,8s/^/        /p' >&2
    fi

    local gst_elements=(
        appsrc appsink capsfilter filesrc filesink fdsink queue tee decodebin
        rtspsrc rtph264depay rtph265depay rtph264pay rtph265pay
        h264parse h265parse mp4mux avmux_mp4
        v4l2src videorate videoscale videoconvert
        jpegparse jpegdec jpegenc x264enc x265enc
    )
    if command -v gst-inspect-1.0 >/dev/null 2>&1; then
        local element
        local missing_gst=()
        for element in "${gst_elements[@]}"; do
            gst-inspect-1.0 "$element" >/dev/null 2>&1 \
                || missing_gst+=("$element")
        done
        if [ "${#missing_gst[@]}" -eq 0 ]; then
            check_pass "通用 GStreamer 解封装、编解码、RTSP 和视频处理元素完整。"
        else
            check_fail "缺少 GStreamer 元素: ${missing_gst[*]}；请补装对应 plugins-base/good/bad/ugly/libav 包。"
        fi
    fi

    local shared_libraries=(
        'libpam\.so' 'libgpiod\.so' 'libgtk-3\.so'
        'libopencv_freetype\.so' 'libgstrtspserver-1\.0\.so'
        'librga\.so' 'librockchip_mpp\.so'
    )
    if command -v ldconfig >/dev/null 2>&1; then
        local library
        local ld_cache
        local missing_libraries=()
        ld_cache="$(ldconfig -p 2>/dev/null || true)"
        for library in "${shared_libraries[@]}"; do
            grep -q "$library" <<< "$ld_cache" \
                || missing_libraries+=("${library//\\/}")
        done
        if [ "${#missing_libraries[@]}" -eq 0 ]; then
            check_pass "PAM、GPIO、GTK、OpenCV freetype、RTSP Server、RGA 和 MPP 共享库可用。"
        else
            check_fail "系统缺少共享库: ${missing_libraries[*]}。"
        fi
    else
        check_fail "系统命令 ldconfig"
    fi

    if [ ! -f /usr/share/fonts/truetype/wqy/wqy-zenhei.ttc ] \
        && [ ! -f /usr/share/fonts/truetype/wqy/wqy-microhei.ttc ] \
        && [ ! -f "$PROJ/vision_analysis/assets/fonts/overlay.ttf" ] \
        && [ ! -f "$PROJ/vision_analysis/assets/fonts/overlay.ttc" ] \
        && [ ! -f "$PROJ/vision_analysis/assets/fonts/overlay.otf" ]; then
        check_fail "中文叠字字体（文泉驿或 assets/fonts）"
    else
        check_pass "中文叠字字体可用。"
    fi

    if [ ! -f "$FRONTEND_DIR/dist/index.html" ]; then
        check_fail "预构建前端 web_console/frontend/dist/index.html"
    else
        check_pass "预构建前端 dist/index.html 存在。"
    fi
    if [ ! -d "$FRONTEND_DIR/node_modules" ]; then
        check_fail "前端 node_modules（离线重建缓存）"
    elif command -v npm >/dev/null 2>&1; then
        local npm_check_output
        if ! npm_check_output="$(cd "$FRONTEND_DIR" && npm ls --depth=0 2>&1)"; then
            check_fail "前端 npm 依赖树与 package-lock.json 不一致"
            echo "      npm 诊断（仅显示前 12 行）：" >&2
            printf '%s\n' "$npm_check_output" | sed -n '1,12s/^/        /p' >&2
            echo "      断网设备请重新运行 offline_install_env_debian/install_offline.sh；不要执行 npm ci/npm install。" >&2
        else
            check_pass "前端 node_modules 与 package-lock.json 一致。"
        fi
    fi

    if [ "$WANT_BUILD" = true ]; then
        local build_errors_before="$CHECK_ERRORS"
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
        if [ "$CHECK_ERRORS" -eq "$build_errors_before" ]; then
            check_pass "C/C++ 编译命令、开发头文件和 pkg-config 模块完整。"
        fi
    fi

    if [ "$CHECK_ERRORS" -eq "$errors_before" ]; then
        check_pass "系统用户态运行环境检查完成，未发现缺失。"
    fi
}

check_one_binary() {
    local label="$1"
    local binary="$2"
    local require_bundled_rknn="$3"
    local binary_dir
    local lib_dir
    local ldd_output=""
    local missing=""
    local resolved_rknn=""
    local manifest=""
    local expected_sha=""
    local actual_sha=""

    [ -x "$binary" ] || return
    binary_dir="$(cd "$(dirname "$binary")" && pwd)"
    lib_dir="$binary_dir/libs"
    if command -v readelf >/dev/null 2>&1; then
        if LC_ALL=C readelf -h "$binary" 2>/dev/null | grep -q 'Machine:.*AArch64'; then
            check_pass "$label 是 AArch64 ELF: $binary"
        else
            check_fail "$label 不是 AArch64 ELF，无法在 RK3588 运行: $binary"
        fi
    fi

    if ! ldd_output="$(LD_LIBRARY_PATH="$lib_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ldd "$binary" 2>&1)"; then
        check_fail "$label 无法解析动态库: $binary；$ldd_output"
        return
    fi
    missing="$(grep 'not found' <<< "$ldd_output" || true)"
    if [ -n "$missing" ]; then
        check_fail "$label 存在未解析动态库: $binary"
        printf '%s\n' "$missing" | sed 's/^/      /' >&2
    else
        check_pass "$label 的动态库均可解析。"
    fi

    if [ "$require_bundled_rknn" != true ]; then
        return
    fi
    if [ ! -f "$lib_dir/librknnrt.so" ]; then
        check_warn "$label 没有随包携带 libs/librknnrt.so，将依赖目标系统版本: $binary"
        return
    fi
    resolved_rknn="$(awk '$1 == "librknnrt.so" && $2 == "=>" {print $3; exit}' <<< "$ldd_output")"
    if [ -n "$resolved_rknn" ] \
            && [ "$(readlink -f "$resolved_rknn" 2>/dev/null || true)" = "$(readlink -f "$lib_dir/librknnrt.so")" ]; then
        check_pass "$label 会加载应用包内固定的 librknnrt.so。"
    else
        check_fail "$label 未解析到应用包内 librknnrt.so；实际路径: ${resolved_rknn:-未找到}。"
    fi

    manifest="$lib_dir/librknnrt.manifest"
    if [ ! -f "$manifest" ]; then
        check_warn "$label 携带了 librknnrt.so，但缺少 librknnrt.manifest，无法追溯版本和哈希。建议重新构建该应用包。"
        return
    fi
    expected_sha="$(manifest_value "$manifest" runtime_sha256)"
    actual_sha="$(sha256sum "$lib_dir/librknnrt.so" | awk '{print $1}')"
    if [ -n "$expected_sha" ] && [ "$actual_sha" = "$expected_sha" ]; then
        check_pass "$label 的包内 librknnrt.so 与 manifest 哈希一致。"
    else
        check_fail "$label 的包内 librknnrt.so 哈希与 manifest 不一致，文件可能被替换或损坏。"
    fi
}

check_application_binaries() {
    local found=0
    local binary
    local app_name

    if [ -x "$PROJ/vision_analysis/vision_analysis" ]; then
        found=$((found + 1))
        check_one_binary "项目调试版 vision_analysis" \
            "$PROJ/vision_analysis/vision_analysis" false
    fi
    if [ -x "$PROJ/vision_analysis/build/vision_analysis" ]; then
        found=$((found + 1))
        check_one_binary "项目构建目录 vision_analysis" \
            "$PROJ/vision_analysis/build/vision_analysis" false
    fi
    if [ -x "$PROJ/vision_analysis/test_skill/vision_analysis" ]; then
        found=$((found + 1))
        check_one_binary "项目应用包 test_skill" \
            "$PROJ/vision_analysis/test_skill/vision_analysis" true
    fi
    if [ -x "$PROJ/first_net_config/first_net_config" ]; then
        found=$((found + 1))
        check_one_binary "首次网络配置程序" \
            "$PROJ/first_net_config/first_net_config" false
    fi

    if [ -d /opt/ai_apps ]; then
        while IFS= read -r -d '' binary; do
            found=$((found + 1))
            app_name="$(basename "$(dirname "$binary")")"
            check_one_binary "已安装应用 $app_name" "$binary" true
        done < <(find /opt/ai_apps -mindepth 2 -maxdepth 2 -type f \
            -name vision_analysis -perm -u+x -print0 2>/dev/null | LC_ALL=C sort -z)
    else
        check_info "/opt/ai_apps 尚不存在；不影响环境准备，安装应用后再复检。"
    fi
    if [ "$found" -eq 0 ]; then
        check_info "没有发现可执行产物，本次只检查环境；构建或安装应用后应再次运行 --check。"
    else
        check_info "共检查 $found 个项目/已安装可执行产物。"
    fi

    local active_pids=""
    active_pids="$(pgrep -x vision_analysis 2>/dev/null | tr '\n' ' ' || true)"
    if [ -n "$active_pids" ]; then
        check_info "当前有 vision_analysis 进程正在运行（PID: $active_pids）；静态检查不会中断它们。"
    else
        check_info "当前没有运行中的 vision_analysis；版本检查不能替代实际模型推理冒烟测试。"
    fi
}

check_environment() {
    echo ">>> [检查 1/5] 系统、架构和已验证平台基线..."
    check_platform_environment
    echo ">>> [检查 2/5] RKNPU、RGA、MPP、DMA 和设备节点..."
    check_rockchip_bsp
    echo ">>> [检查 3/5] 系统命令、Python、前端和通用依赖..."
    check_userland_environment
    echo ">>> [检查 4/5] Rockchip GStreamer 硬件插件及回退能力..."
    if command -v gst-inspect-1.0 >/dev/null 2>&1; then
        check_rockchip_gstreamer
    else
        check_fail "缺少 gst-inspect-1.0，无法检查 Rockchip MPP 插件。"
    fi
    echo ">>> [检查 5/5] 项目和已安装应用的 ELF、动态库与固定 RKNN Runtime..."
    check_application_binaries

    echo "------------------------------------------------------------"
    echo "检查统计: 通过 $CHECK_PASSES 项，警告 $CHECK_WARNINGS 项，失败 $CHECK_ERRORS 项。"
}

if [ "$CHECK_ONLY" != true ]; then
    install_apt_dependencies
    install_node
    install_python_dependencies
    prepare_frontend
fi

check_environment

echo ""
if [ "$CHECK_ERRORS" -ne 0 ]; then
    echo "[失败] 全面环境检测发现 $CHECK_ERRORS 项致命问题、$CHECK_WARNINGS 项兼容性警告。" >&2
    echo "  请先处理上方 [失败] 项；仅修改或忽略校验输出不能保证程序可以运行。" >&2
    echo "  本检查不会启动摄像头或模型，修复后还应执行一次实际视频/NPU推理冒烟测试。" >&2
    exit 1
fi

if [ "$CHECK_ONLY" = true ]; then
    if [ "$WANT_BUILD" = true ]; then
        echo "[OK] 运行环境及 C/C++ 编译环境未发现致命问题。"
    else
        echo "[OK] 运行环境未发现致命问题。"
    fi
    echo "  本次仅执行检查：未联网、未安装软件、未修改系统。"
    if [ "$CHECK_WARNINGS" -ne 0 ]; then
        echo "  当前有 $CHECK_WARNINGS 项警告；程序可能可以运行，但相关功能或版本组合尚未完全验证。"
    else
        echo "  当前系统符合项目记录的静态环境基线。"
    fi
    echo "  静态检查通过后，仍应使用实际摄像头、模型和输出链路做现场冒烟测试。"
else
    echo "[OK] 第三方环境安装完成，全面检测未发现致命问题。"
    [ "$CHECK_WARNINGS" -eq 0 ] \
        || echo "  仍有 $CHECK_WARNINGS 项兼容性警告，请查看上方提示并完成硬件冒烟测试。"
    echo "  到达断网现场后，可运行以下命令复检环境："
    echo "    bash install_deps.sh --check$([ "$WANT_BUILD" = true ] && echo ' --build' || true)"
    echo "  仅在需要重新部署 Web 控制台时，才运行："
    echo "    sudo env OFFLINE=1 bash web_console/install.sh"
fi
echo "  说明：Rockchip RKNPU内核驱动/RGA/MPP 由厂家系统提供；librknnrt.so 由应用包固定。"
