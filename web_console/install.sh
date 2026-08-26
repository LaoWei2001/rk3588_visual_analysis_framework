#!/bin/bash
# RK3588 Web Console 安装脚本
# 用法：把整个 web_console 文件夹复制到 RK3588，然后在板子上执行此脚本
#   scp -r web_console root@<板子IP>:~
#   ssh root@<板子IP> "cd ~/web_console && bash install.sh"
set -e

# 可移植安装路径；迁移到其他目录时可执行
#   APPS_ROOT=/data/ai_apps bash install.sh
APPS_ROOT="${APPS_ROOT:-/opt/ai_apps}"
INSTALL_DIR="${INSTALL_DIR:-$APPS_ROOT/_console}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON_BIN="$(command -v python3 || true)"

if [ -z "$PYTHON_BIN" ]; then
    echo "[错误] 未找到 python3"
    exit 1
fi

echo "=== RK3588 Web Console 安装 ==="

# 设为 OFFLINE=1 时只使用 install_deps.sh 已准备好的 Python 环境和预构建 dist，
# 绝不访问 PyPI/npm；适合现场无公网安装。
OFFLINE="${OFFLINE:-0}"

# 1. 安装后端
echo "[1/4] 安装后端..."
mkdir -p "$INSTALL_DIR/backend"
cp -r "$SCRIPT_DIR/backend/"* "$INSTALL_DIR/backend/"
cd "$INSTALL_DIR/backend"
if [ "$OFFLINE" = "1" ]; then
    "$PYTHON_BIN" -m pip install --no-index -r requirements.txt --quiet
else
    "$PYTHON_BIN" -m pip install -r requirements.txt --quiet
fi

# 2. 构建 / 复制前端
echo "[2/4] 处理前端..."
mkdir -p "$INSTALL_DIR/frontend"

if [ "$OFFLINE" = "1" ]; then
    if [ ! -f "$SCRIPT_DIR/frontend/dist/index.html" ]; then
        echo "  [错误] OFFLINE=1 但缺少预构建 frontend/dist/index.html"
        echo "         请在有公网时先运行项目根目录 install_deps.sh。"
        exit 1
    fi
    cp -r "$SCRIPT_DIR/frontend/dist" "$INSTALL_DIR/frontend/"
    echo "    离线模式：已复制预构建前端"
elif command -v node &>/dev/null && command -v npm &>/dev/null; then
    echo "    检测到 Node.js $(node -v)，直接在板端构建..."
    cd "$SCRIPT_DIR/frontend"
    npm ci --no-audit --no-fund
    npm run build
    cp -r dist "$INSTALL_DIR/frontend/"
    echo "    构建完成"
elif [ -d "$SCRIPT_DIR/frontend/dist" ]; then
    echo "    未找到 Node.js，使用已有的 dist/ 构建产物..."
    cp -r "$SCRIPT_DIR/frontend/dist" "$INSTALL_DIR/frontend/"
    echo "    前端构建产物已复制"
else
    echo ""
    echo "  [错误] 既没有 Node.js 也没有预构建的 dist/ 目录"
    echo "  解决方案二选一："
    echo "    方案 A：在板子上安装 Node.js，然后重新运行此脚本"
    echo "      curl -fsSL https://deb.nodesource.com/setup_20.x | bash -"
    echo "      apt-get install -y nodejs"
    echo ""
    echo "    方案 B：在开发机上先构建，再整体复制到板子"
    echo "      cd frontend && npm install && npm run build"
    exit 1
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
