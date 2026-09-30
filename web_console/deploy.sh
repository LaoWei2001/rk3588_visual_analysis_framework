#!/usr/bin/env bash
# RK3588 Web Console 内部部署脚本。
# 只消费已经准备好的 Python 环境和前端产物；正常部署请从项目根目录执行 ./install.sh。
set -Eeuo pipefail

if [ "$#" -ne 0 ]; then
    echo "[错误] web_console/deploy.sh 是内部部署器，不接受参数。" >&2
    echo "       请在项目根目录执行 sudo ./install.sh <online|offline>。" >&2
    exit 2
fi

# 可移植安装路径；由根安装器透传环境变量。
#   sudo APPS_ROOT=/data/ai_apps ./install.sh online
APPS_ROOT="${APPS_ROOT:-/opt/ai_apps}"
INSTALL_DIR="${INSTALL_DIR:-$APPS_ROOT/_console}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PYTHON_BIN="/usr/bin/python3"
DIST_STAGE=""

cleanup() {
    [ -z "$DIST_STAGE" ] || rm -rf -- "$DIST_STAGE"
}
trap cleanup EXIT

if [ "$(id -u)" -ne 0 ]; then
    echo "[错误] 安装 systemd 服务需要 root 权限。"
    echo "       联网安装请执行: sudo $PROJECT_ROOT/install.sh online"
    echo "       离线部署请执行: sudo $PROJECT_ROOT/install.sh offline"
    exit 1
fi

echo "=== RK3588 Web Console 部署 ==="

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "[错误] 未找到 ffmpeg，网页实时画面无法进行 RTSP 零转码封装。" >&2
    echo "       请先在项目根目录运行 install_deps.sh。" >&2
    exit 1
fi

[ -x "$PYTHON_BIN" ] \
    || { echo "[错误] 未找到系统 Python: $PYTHON_BIN" >&2; exit 1; }

# 1. 校验并部署后端。依赖安装统一由 install_deps.sh 或完整离线包负责。
echo "[1/4] 校验并部署后端..."
if ! "$PYTHON_BIN" - <<'PY'
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
    echo "[错误] 系统 Python 环境不完整。" >&2
    echo "       请从项目根目录重新执行 sudo ./install.sh online，或安装完整离线包。" >&2
    exit 1
fi
if ! "$PYTHON_BIN" -m pip check; then
    echo "[错误] 系统 Python 环境存在依赖冲突。" >&2
    echo "       请从项目根目录重新执行 sudo ./install.sh online，或修复离线环境。" >&2
    exit 1
fi
rm -rf -- "$INSTALL_DIR/backend"
mkdir -p "$INSTALL_DIR/backend"
cp -a "$SCRIPT_DIR/backend/." "$INSTALL_DIR/backend/"

# 2. 部署根安装器或离线包已经准备好的前端。
echo "[2/4] 部署预构建前端..."
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

FRONTEND_DIST="$SCRIPT_DIR/frontend/dist"
if [ ! -f "$FRONTEND_DIST/index.html" ]; then
    FRONTEND_DIST="/usr/share/vision-analysis/frontend/dist"
fi
if [ ! -f "$FRONTEND_DIST/index.html" ]; then
    echo "  [错误] 没有找到预构建前端。" >&2
    echo "         请从项目根目录重新执行 sudo ./install.sh online，或安装完整离线包。" >&2
    exit 1
fi
deploy_frontend_dist "$FRONTEND_DIST"
echo "    已部署：$FRONTEND_DIST"

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

# 3. 安装 Web systemd 服务
echo "[3/4] 安装 Web systemd 服务..."
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
    printf 'Environment="VISION_PYTHON=%s"\n' "$PYTHON_BIN"
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
echo "[4/4] 启动 Web 服务..."
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
echo "  Python: $PYTHON_BIN（系统环境）"
