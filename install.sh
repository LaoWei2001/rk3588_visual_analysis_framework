#!/usr/bin/env bash
# RK3588 视觉分析框架统一管理入口。
# 用户只需要从项目根目录调用本脚本；各组件目录中的安装器是内部实现与故障修复入口。
set -Eeuo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONSOLE_UNIT="rk3588-console.service"
GPIO_CONTROL_UNIT="rk3588-gpio-control.service"
GPIO_RESTORE_UNIT="rk3588-gpio-restore.service"

usage() {
    cat <<'EOF'
用法：
  sudo ./install.sh online              联网准备全部依赖并安装/升级平台
  sudo ./install.sh offline             使用已准备好的离线环境和前端产物安装平台
  sudo ./install.sh upgrade [模式]      升级平台；模式为 online 或 offline，默认 offline
  ./install.sh status                   查看平台、GPIO和Web服务状态
  sudo ./install.sh uninstall [--yes]   卸载平台服务，保留应用和GPIO持久状态

说明：
  online 适合仍可访问 APT、PyPI 和 npm 镜像的新设备。
  offline 适合已经由完整离线包准备好依赖、源码和前端产物的设备。
  完整离线 bundle 首次部署仍直接运行 bundle 根目录的 install_offline.sh。
EOF
}

require_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "[错误] 此操作需要 root 权限，请使用 sudo 重新执行。" >&2
        exit 1
    fi
}

unit_summary() {
    local unit="$1"
    local enabled="未安装"
    local active="未安装"
    if systemctl cat "$unit" >/dev/null 2>&1; then
        enabled="$(systemctl is-enabled "$unit" 2>/dev/null || true)"
        active="$(systemctl is-active "$unit" 2>/dev/null || true)"
        [ -n "$enabled" ] || enabled="unknown"
        [ -n "$active" ] || active="unknown"
    fi
    printf '  %-36s 开机=%-10s 当前=%s\n' "$unit" "$enabled" "$active"
}

show_status() {
    echo "RK3588 视觉分析框架状态"
    echo "----------------------------------------"
    if ! command -v systemctl >/dev/null 2>&1; then
        echo "  当前系统没有 systemctl，无法查询系统服务。"
        return 0
    fi
    unit_summary "$CONSOLE_UNIT"
    unit_summary "$GPIO_CONTROL_UNIT"
    unit_summary "$GPIO_RESTORE_UNIT"
    echo
    if [ -x /usr/local/bin/rk3588-gpioctl ]; then
        echo "  GPIO控制工具：/usr/local/bin/rk3588-gpioctl"
    elif [ -x /usr/local/bin/gpio_test ]; then
        echo "  GPIO控制工具：/usr/local/bin/gpio_test（旧版兼容安装）"
    else
        echo "  GPIO控制工具：未安装"
    fi
    if [ -S /run/rk3588-gpio-control/control.sock ]; then
        echo "  GPIO控制Socket：已就绪"
    else
        echo "  GPIO控制Socket：未就绪"
    fi
    if [ -e /run/rk3588-gpio-persistence/enabled ]; then
        echo "  GPIO电平保持：已开启"
    else
        echo "  GPIO电平保持：已关闭"
    fi
}

check_offline_source_environment() {
    local command_name
    for command_name in cmake pkg-config cc; do
        if ! command -v "$command_name" >/dev/null 2>&1; then
            echo "[错误] 离线源码安装缺少构建命令：$command_name" >&2
            echo "       请先使用完整离线 bundle 的 install_offline.sh 准备设备。" >&2
            exit 1
        fi
    done
    if [ ! -f "$PROJECT_ROOT/web_console/frontend/dist/index.html" ] \
            && [ ! -f /usr/share/vision-analysis/frontend/dist/index.html ]; then
        echo "[错误] 没有找到离线Web前端产物。" >&2
        echo "       请使用完整离线 bundle，或在有网环境改用 online。" >&2
        exit 1
    fi
}

install_platform() {
    local mode="$1"
    require_root
    echo "============================================================"
    echo "  RK3588 视觉分析框架统一安装"
    echo "  项目目录：$PROJECT_ROOT"
    echo "  安装模式：$mode"
    echo "============================================================"

    if [ "$mode" = online ]; then
        echo ">>> [1/2] 准备项目全部依赖和预构建前端..."
        bash "$PROJECT_ROOT/install_deps.sh"
    else
        echo ">>> [1/2] 检查已经准备好的离线环境..."
        check_offline_source_environment
    fi

    # online 阶段已经统一安装 Python/npm 依赖并生成 dist；这里使用 offline 模式部署，
    # 避免 Web 子安装器再次访问网络、重复 npm ci。GPIO服务也由它在最早阶段安装。
    echo ">>> [2/2] 安装GPIO平台服务和Web控制台..."
    bash "$PROJECT_ROOT/web_console/install.sh" offline

    echo
    echo "[完成] 平台安装成功。以后统一使用："
    echo "  $PROJECT_ROOT/install.sh status"
    show_status
}

confirm_uninstall() {
    local assume_yes="$1"
    [ "$assume_yes" = true ] && return 0
    if [ ! -t 0 ]; then
        echo "[错误] 非交互卸载必须明确传入 --yes。" >&2
        exit 2
    fi
    echo "即将停止并移除Web控制台与GPIO平台服务。"
    echo "GPIO被释放后会回到板载上拉/外接下拉决定的电平。"
    echo "视觉应用目录和 /var/lib/rk3588-gpio 持久状态会保留。"
    read -r -p "请输入 UNINSTALL 继续：" answer
    [ "$answer" = UNINSTALL ] || { echo "已取消。"; exit 0; }
}

uninstall_platform() {
    local assume_yes="$1"
    require_root
    confirm_uninstall "$assume_yes"

    systemctl disable --now "$CONSOLE_UNIT" >/dev/null 2>&1 || true
    systemctl disable --now "$GPIO_RESTORE_UNIT" >/dev/null 2>&1 || true
    systemctl disable --now "$GPIO_CONTROL_UNIT" >/dev/null 2>&1 || true

    rm -f \
        "/etc/systemd/system/$CONSOLE_UNIT" \
        "/etc/systemd/system/$GPIO_CONTROL_UNIT" \
        "/etc/systemd/system/$GPIO_RESTORE_UNIT" \
        /usr/local/bin/rk3588-gpioctl \
        /usr/local/bin/gpio_test \
        /usr/local/sbin/rk3588-gpio-daemon \
        /usr/local/sbin/rk3588_gpio_control_daemon
    systemctl daemon-reload

    # 只处理默认且精确的控制台目录；用户安装的视觉应用和 .data 均不删除。
    if [ -d /opt/ai_apps/_console ]; then
        local backup="/opt/ai_apps/_console.uninstalled.$(date +%Y%m%d%H%M%S)"
        mv /opt/ai_apps/_console "$backup"
        echo "Web控制台目录已移动到可恢复备份：$backup"
    fi
    echo "平台服务已卸载；视觉应用和GPIO持久状态已保留。"
}

command_name="${1:-}"
[ "$#" -eq 0 ] || shift
case "$command_name" in
    online|offline)
        [ "$#" -eq 0 ] || { echo "[错误] $command_name 不接受额外参数。" >&2; exit 2; }
        install_platform "$command_name"
        ;;
    upgrade)
        mode="${1:-offline}"
        [ "$#" -eq 0 ] || shift
        [ "$#" -eq 0 ] || { echo "[错误] upgrade 参数过多。" >&2; exit 2; }
        case "$mode" in
            online|offline) install_platform "$mode" ;;
            *) echo "[错误] upgrade 模式只能是 online 或 offline。" >&2; exit 2 ;;
        esac
        ;;
    status)
        [ "$#" -eq 0 ] || { echo "[错误] status 不接受额外参数。" >&2; exit 2; }
        show_status
        ;;
    uninstall)
        assume_yes=false
        if [ "${1:-}" = --yes ]; then
            assume_yes=true
            shift
        fi
        [ "$#" -eq 0 ] || { echo "[错误] uninstall 只接受可选参数 --yes。" >&2; exit 2; }
        uninstall_platform "$assume_yes"
        ;;
    -h|--help|help)
        usage
        ;;
    "")
        usage >&2
        exit 2
        ;;
    *)
        echo "[错误] 未知命令：$command_name" >&2
        usage >&2
        exit 2
        ;;
esac
