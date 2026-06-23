#!/bin/bash
# RK3588 Web Console 安装脚本
# 用法：把整个 web_console 文件夹复制到 RK3588，然后在板子上执行此脚本
#   scp -r web_console root@<板子IP>:~
#   ssh root@<板子IP> "cd ~/web_console && bash install.sh"
set -e

INSTALL_DIR=/opt/ai_apps/_console
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=== RK3588 Web Console 安装 ==="

# 1. 安装后端
echo "[1/4] 安装后端..."
mkdir -p "$INSTALL_DIR/backend"
cp -r "$SCRIPT_DIR/backend/"* "$INSTALL_DIR/backend/"
cd "$INSTALL_DIR/backend"
pip3 install -r requirements.txt --quiet

# 2. 构建 / 复制前端
echo "[2/4] 处理前端..."
mkdir -p "$INSTALL_DIR/frontend"

if command -v node &>/dev/null && command -v npm &>/dev/null; then
    echo "    检测到 Node.js $(node -v)，直接在板端构建..."
    cd "$SCRIPT_DIR/frontend"
    npm install --silent
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
cp "$SCRIPT_DIR/rk3588-console.service" /etc/systemd/system/
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
