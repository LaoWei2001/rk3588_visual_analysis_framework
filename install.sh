#!/usr/bin/env bash
# RK3588 视觉分析框架统一管理入口。
# 用户只需要从项目根目录调用本脚本；各组件目录中的安装器是内部实现与故障修复入口。
set -Eeuo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONSOLE_UNIT="rk3588-console.service"
GPIO_CONTROL_UNIT="rk3588-gpio-control.service"
GPIO_RESTORE_UNIT="rk3588-gpio-restore.service"
GPIO_SERVICE_INSTALLER="$PROJECT_ROOT/services/framework/gpio_state/install.sh"
WEB_CONSOLE_DEPLOYER="$PROJECT_ROOT/web_console/deploy.sh"

usage() {
    cat <<'EOF'
用法：
  sudo ./install.sh online              联网准备全部依赖并部署平台
  sudo ./install.sh offline             使用已有依赖构建前端并离线部署平台
  ./install.sh status                   查看平台、GPIO和Web服务状态
  sudo ./install.sh uninstall [--yes]   卸载平台服务，保留应用和GPIO持久状态

说明：
  online 适合仍可访问 APT、PyPI 和 npm 镜像的新设备。
  offline 适合已经准备好依赖的设备；缺少前端产物时使用本地依赖构建。
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

prepare_offline_frontend() {
    local frontend_dir="$PROJECT_ROOT/web_console/frontend"
    local source_version=""
    [ ! -f "$PROJECT_ROOT/.vision-analysis-source-version" ] \
        || source_version="$(cat "$PROJECT_ROOT/.vision-analysis-source-version")"

    [ ! -f "$frontend_dir/dist/index.html" ] || return 0

    # 兼容已经提供预构建页面、没有本地前端依赖的旧完整包。
    if [[ "$source_version" != *+src ]] \
            && [ ! -d "$frontend_dir/node_modules" ] \
            && [ -f /usr/share/vision-analysis/frontend/dist/index.html ]; then
        return 0
    fi

    local dependency_restorer="$PROJECT_ROOT/tools/offline_dev_install/debian/restore_frontend_dependencies.sh"
    if [ -f "$dependency_restorer" ]; then
        sh "$dependency_restorer" "$PROJECT_ROOT" || exit 1
    fi

    if [ -d "$frontend_dir/node_modules" ]; then
        local command_name
        for command_name in node npm; do
            if ! command -v "$command_name" >/dev/null 2>&1; then
                echo "[错误] 缺少前端构建命令：$command_name。" >&2
                echo "       请检查设备已安装的 Node.js/npm 是否在 PATH 中。" >&2
                exit 1
            fi
        done
        echo ">>> 新版源码尚未构建前端，使用已有 node_modules 离线构建..."
        if ! (cd "$frontend_dir" && npm --offline --no-audit --no-fund run build); then
            echo "[错误] 前端离线构建失败，请检查上面的具体错误；平台尚未部署。" >&2
            exit 1
        fi
        if [ ! -f "$frontend_dir/dist/index.html" ]; then
            echo "[错误] 前端构建没有生成 dist/index.html；平台尚未部署。" >&2
            exit 1
        fi
        return 0
    fi

    echo "[错误] 新版源码没有前端产物，也没有可用的本地前端依赖：$frontend_dir/node_modules。" >&2
    echo "       源码更新会复用旧源码的 node_modules，请检查旧目录是否仍存在、依赖链接是否有效。" >&2
    echo "       依赖已准备好后，在本目录重新执行 sudo ./install.sh offline 即可。" >&2
    exit 1
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
    prepare_offline_frontend
}

install_platform() {
    local mode="$1"
    require_root
    echo "============================================================"
    echo "  RK3588 视觉分析框架统一安装"
    echo "  项目目录：$PROJECT_ROOT"
    echo "  安装模式：$mode"
    echo "============================================================"

    if [ ! -x "$GPIO_SERVICE_INSTALLER" ]; then
        echo "[错误] 缺少可执行的 GPIO 服务安装器：$GPIO_SERVICE_INSTALLER" >&2
        exit 1
    fi
    if [ ! -x "$WEB_CONSOLE_DEPLOYER" ]; then
        echo "[错误] 缺少可执行的 Web 控制台部署器：$WEB_CONSOLE_DEPLOYER" >&2
        exit 1
    fi

    if [ "$mode" = online ]; then
        echo ">>> [1/3] 准备项目全部依赖和预构建前端..."
        bash "$PROJECT_ROOT/install_deps.sh"
    else
        echo ">>> [1/3] 检查已经准备好的离线环境..."
        check_offline_source_environment
    fi

    echo ">>> [2/3] 安装 GPIO 控制与电平保持服务..."
    bash "$GPIO_SERVICE_INSTALLER"

    echo ">>> [3/3] 部署 Web 控制台..."
    bash "$WEB_CONSOLE_DEPLOYER"

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
