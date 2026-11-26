#!/usr/bin/env bash
# 从当前源码构建并发布已安装的 Web 控制台前端。
set -euo pipefail

usage() {
    cat <<'EOF'
用法：./update_webconsole.sh [--port 端口]

修改网页后运行此命令。脚本使用已有 npm 依赖构建前端，再将页面发布到板端。
默认沿用当前端口，发布后刷新浏览器即可。
例如：sudo ./update_webconsole.sh --port 9090。修改端口时会重启 Web 服务。

默认控制台目录：/opt/ai_apps/_console
自定义安装目录时，沿用安装时的 INSTALL_DIR 或 APPS_ROOT。
EOF
}

if [[ $# -eq 1 && ( $1 == --help || $1 == -h ) ]]; then
    usage
    exit 0
fi

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
frontend_dir="$project_dir/web_console/frontend"
console_dir="${INSTALL_DIR:-${APPS_ROOT:-/opt/ai_apps}/_console}"
target="$console_dir/frontend/dist"

console_port=""
port_helper="$project_dir/web_console/scripts/console_port.py"
if [[ $# -gt 0 ]]; then
    console_port="$(python3 "$port_helper" select "$@")"
    if [[ $EUID -ne 0 ]]; then
        echo "[错误] 修改 Web 服务端口需要 root，请使用 sudo 运行。" >&2
        exit 1
    fi
fi

if [[ ! -f "$target/index.html" ]]; then
    echo "[错误] 未找到已安装的网页：$target/index.html。请检查安装目录；首次安装平台请使用 install.sh。" >&2
    exit 1
fi
if [[ "$target" -ef "$frontend_dir/dist" ]]; then
    echo "[错误] 发布目录与源码的构建目录相同，请检查 INSTALL_DIR。" >&2
    exit 1
fi
if [[ ! -w "$target" ]]; then
    echo "[错误] 无权写入 $target，请使用有写权限的账户运行。" >&2
    exit 1
fi
for tool in node npm rsync flock; do
    if ! command -v "$tool" >/dev/null; then
        echo "[错误] 缺少命令 $tool，请先准备前端构建和发布环境。" >&2
        exit 1
    fi
done
if [[ ! -d "$frontend_dir/node_modules" ]]; then
    echo "[错误] 缺少前端依赖。请先在 $frontend_dir 执行 npm ci，或恢复离线依赖。" >&2
    exit 1
fi

# 同一份源码的并发构建会同时清理 dist，锁住整个构建和发布过程。
exec 9<"$frontend_dir"
if ! flock -n 9; then
    echo "[错误] 前端正在更新，请等本次更新完成。" >&2
    exit 1
fi

echo "[1/2] 构建最新前端页面…"
npm --prefix "$frontend_dir" run build
dist="$frontend_dir/dist"
if [[ ! -s "$dist/index.html" || ! -d "$dist/assets" ]]; then
    echo "[错误] 构建产物不完整，请检查 $dist。" >&2
    exit 1
fi

echo "[2/2] 发布到 $target …"
# 先发布资源，最后替换入口。保留旧资源供已打开的网页继续加载。
rsync -a --exclude='/index.html' -- "$dist/" "$target/"
if [[ -n $console_port ]]; then
    python3 "$port_helper" update --port "$console_port" >/dev/null
fi
rsync -a --checksum -- "$dist/index.html" "$target/"
if [[ -n $console_port ]]; then
    echo "[完成] 网页已更新，访问地址：http://设备IP:$console_port"
else
    echo "[完成] 网页已更新，请刷新浏览器。"
fi
