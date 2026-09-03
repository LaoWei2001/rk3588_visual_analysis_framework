#!/usr/bin/env bash
# RK3588 Web Console 安装脚本
# 用法：把整个 web_console 文件夹复制到 RK3588，然后在板子上执行此脚本
#   scp -r web_console root@<板子IP>:~
#   ssh root@<板子IP> "cd ~/web_console && bash install.sh"       # 默认联网安装
#   ssh root@<板子IP> "cd ~/web_console && OFFLINE=1 bash install.sh"  # 明确离线部署
set -Eeuo pipefail

# 可移植安装路径；迁移到其他目录时可执行
#   APPS_ROOT=/data/ai_apps bash install.sh
APPS_ROOT="${APPS_ROOT:-/opt/ai_apps}"
INSTALL_DIR="${INSTALL_DIR:-$APPS_ROOT/_console}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON_BIN="$(command -v python3 || true)"
FRONTEND_BUILD_DIR=""
DIST_STAGE=""

cleanup() {
    [ -z "$FRONTEND_BUILD_DIR" ] || rm -rf -- "$FRONTEND_BUILD_DIR"
    [ -z "$DIST_STAGE" ] || rm -rf -- "$DIST_STAGE"
}
trap cleanup EXIT

if [ "$(id -u)" -ne 0 ]; then
    echo "[错误] 安装 systemd 服务需要 root 权限。"
    echo "       联网安装请执行: sudo bash $SCRIPT_DIR/install.sh"
    echo "       离线部署请执行: sudo env OFFLINE=1 bash $SCRIPT_DIR/install.sh"
    exit 1
fi

if [ -z "$PYTHON_BIN" ]; then
    echo "[错误] 未找到 python3"
    exit 1
fi

echo "=== RK3588 Web Console 安装 ==="

# 模式必须由用户明确决定：默认始终联网安装；只有 OFFLINE=1 才禁止 pip/npm 联网。
# frontend/dist 是否存在只代表项目带有预构建产物，绝不能作为网络状态判断依据。
OFFLINE="${OFFLINE:-0}"
case "$OFFLINE" in
    0|1) ;;
    *) echo "[错误] OFFLINE 只能是 0 或 1，当前值: $OFFLINE" >&2; exit 2 ;;
esac
if [ "$OFFLINE" = "1" ]; then
    echo "    模式: 离线部署（仅验证已安装的 Python 环境并复制预构建前端）"
else
    echo "    模式: 联网安装（使用 pip 软件源和 npm registry 重新安装、构建）"
fi

# 1. 安装后端
echo "[1/4] 安装后端..."
PIP_SYSTEM_ARGS=()
PIP_INSTALL_HELP="$($PYTHON_BIN -m pip help install 2>/dev/null || true)"
if grep -q -- '--break-system-packages' <<< "$PIP_INSTALL_HELP"; then
    PIP_SYSTEM_ARGS+=(--break-system-packages)
fi
if [ "$OFFLINE" = "1" ]; then
    if ! "$PYTHON_BIN" - <<'PY'
import importlib
import sys

modules = (
    "fastapi", "starlette", "uvicorn", "pydantic", "aiofiles", "multipart",
    "uvloop", "httptools", "watchfiles", "dotenv", "cv2", "pam", "six",
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
        echo "[错误] 离线 Python 环境不完整。" >&2
        echo "       请先运行 offline_install_env_debian/install_offline.sh，再重新部署 Web 控制台。" >&2
        exit 1
    fi
    if ! "$PYTHON_BIN" -m pip check; then
        echo "[错误] 离线 Python 环境存在依赖冲突。" >&2
        echo "       请先运行 offline_install_env_debian/install_offline.sh 修复环境。" >&2
        exit 1
    fi
    echo "    离线模式：Python 模块和依赖关系检查通过，未执行 pip install。"
else
    "$PYTHON_BIN" -m pip install "${PIP_SYSTEM_ARGS[@]}" \
        -r "$SCRIPT_DIR/backend/requirements.txt" --quiet
fi
rm -rf -- "$INSTALL_DIR/backend"
mkdir -p "$INSTALL_DIR/backend"
cp -a "$SCRIPT_DIR/backend/." "$INSTALL_DIR/backend/"

# 2. 构建 / 复制前端
echo "[2/4] 处理前端..."
mkdir -p "$INSTALL_DIR/frontend"

deploy_frontend_dist() {
    local source_dist="$1"
    DIST_STAGE="$(mktemp -d "$INSTALL_DIR/frontend/.dist-install.XXXXXX")"
    cp -a "$source_dist" "$DIST_STAGE/dist"
    rm -rf -- "$INSTALL_DIR/frontend/dist"
    mv "$DIST_STAGE/dist" "$INSTALL_DIR/frontend/dist"
    rmdir "$DIST_STAGE"
    DIST_STAGE=""
}

if [ "$OFFLINE" = "1" ]; then
    if [ ! -f "$SCRIPT_DIR/frontend/dist/index.html" ]; then
        echo "  [错误] OFFLINE=1 但缺少预构建 frontend/dist/index.html"
        echo "         请重新运行 offline_install_env_debian/install_offline.sh，或在有公网时运行 install_deps.sh。"
        exit 1
    fi
    deploy_frontend_dist "$SCRIPT_DIR/frontend/dist"
    echo "    离线模式：已复制预构建前端"
else
    if ! command -v node &>/dev/null || ! command -v npm &>/dev/null; then
        echo "  [错误] 联网模式需要 Node.js 和 npm 来执行锁定构建。" >&2
        echo "         请先在项目根目录运行 install_deps.sh，然后重新执行本脚本。" >&2
        echo "         如果目标设备没有网络，请明确使用 OFFLINE=1。" >&2
        exit 1
    fi
    echo "    检测到 Node.js $(node -v)，在临时目录构建..."
    FRONTEND_BUILD_DIR="$(mktemp -d)"
    (
        cd "$SCRIPT_DIR/frontend"
        tar --exclude='./node_modules' --exclude='./dist' --exclude='*.tsbuildinfo' -cf - .
    ) | tar -xf - -C "$FRONTEND_BUILD_DIR"
    (
        cd "$FRONTEND_BUILD_DIR"
        npm ci --no-audit --no-fund
        npm run build
    )
    deploy_frontend_dist "$FRONTEND_BUILD_DIR/dist"
    echo "    构建完成"
fi

# 可替换的图片文件（不存在也没关系）
for imgfile in logo.png img.png; do
    src="$SCRIPT_DIR/frontend/$imgfile"
    if [ -f "$src" ]; then
        cp "$src" "$INSTALL_DIR/frontend/$imgfile"
        echo "    已复制 $imgfile"
    fi
done

# 随机 logo 目录：用户把图片/GIF 放进 frontend/logos/，每次打开网页随机取一张。
# 用 -n（no-clobber）合并：重装时【保留】板子上已放的图片，只补缺失的文件
# （首装时播种 logo.png + README；之后你在板子上加的图不会被覆盖）。
if [ -d "$SCRIPT_DIR/frontend/logos" ]; then
    cp -rn "$SCRIPT_DIR/frontend/logos" "$INSTALL_DIR/frontend/"
    echo "    已准备随机 logo 目录: $INSTALL_DIR/frontend/logos/  (把图片/GIF 放这里)"
fi

# 3. 安装 systemd 服务
echo "[3/4] 安装 systemd 服务..."
{
    echo "[Unit]"
    echo "Description=RK3588 Web Config Console"
    echo "After=network-online.target"
    echo "Wants=network-online.target"
    echo ""
    echo "[Service]"
    echo "Type=simple"
    echo "User=root"
    printf 'WorkingDirectory=%s\n' "$INSTALL_DIR/backend"
    printf 'Environment="APPS_ROOT=%s"\n' "$APPS_ROOT"
    echo 'Environment="BINARY_NAME=vision_analysis"'
    echo "ExecStartPre=-/usr/bin/nm-online -q --timeout=30"
    printf 'ExecStart="%s" -m uvicorn main:app --host 0.0.0.0 --port 8080 --workers 1 --log-level info\n' "$PYTHON_BIN"
    echo "Restart=always"
    echo "RestartSec=5"
    echo "StandardOutput=journal"
    echo "StandardError=journal"
    echo "SyslogIdentifier=rk3588-console"
    echo "KillMode=control-group"
    echo "KillSignal=SIGTERM"
    echo "TimeoutStopSec=10"
    echo ""
    echo "[Install]"
    echo "WantedBy=multi-user.target"
} > /etc/systemd/system/rk3588-console.service
# 旧安装可能遗留同名路径覆盖文件；必须移除，保证上面的完整 unit 是唯一配置源。
rm -f /etc/systemd/system/rk3588-console.service.d/paths.conf
rmdir /etc/systemd/system/rk3588-console.service.d 2>/dev/null || true
systemctl daemon-reload
systemctl enable rk3588-console

# 4. 启动
echo "[4/4] 启动服务..."
systemctl restart rk3588-console
sleep 2
systemctl status rk3588-console --no-pager

LAN_IP=$(ip route get 8.8.8.8 2>/dev/null | awk '{for(i=1;i<=NF;i++){if($i=="src"){print $(i+1);exit}}}')
if [ -z "$LAN_IP" ]; then
    LAN_IP=$(hostname -I | awk '{print $NF}')
fi
echo ""
echo "✓ 安装完成！访问地址: http://${LAN_IP}:8080"
echo "  程序根目录: $APPS_ROOT"
echo "  控制台目录: $INSTALL_DIR"
