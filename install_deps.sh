#!/bin/bash
# ============================================================================
# RK3588 一键依赖安装脚本（Debian/Ubuntu/Armbian 系）
#
# 用途：在一块全新的 RK3588 上快速装齐运行本项目所需的 apt + Node + pip 组件。
# 用法：
#   bash install_deps.sh              # 运行时依赖（部署预编译 dist 时用这个就够）
#   bash install_deps.sh --build      # 额外安装板端从源码编译主程序所需的 -dev 包
#
# 说明：
#   - Rockchip 厂商运行库 librknnrt.so / librga.so.2 / libhiredis.so.* 由 build.sh
#     打包进 dist/libs/（仓库 rk3588_yolo/test1/libs 里有现成的），不走 apt。
#   - 主程序通过 LD_LIBRARY_PATH=dist/libs 加载上述库；GStreamer 插件是运行时
#     动态加载、不会被打包，所以必须 apt 装插件。
# ============================================================================
set -e

WANT_BUILD=false
[ "$1" = "--build" ] && WANT_BUILD=true

PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "============================================================"
echo "  RK3588 依赖安装  (项目根: $PROJ)"
echo "  模式: $([ "$WANT_BUILD" = true ] && echo '运行时 + 编译' || echo '仅运行时')"
echo "============================================================"

# ---- [1] APT 运行时依赖 ----
echo ">>> [1/5] 安装 APT 运行时依赖..."
# 用 || true：某些板子 sources.list 里有失效的第三方/backports 源(如 bullseye-backports 已下架,
# 报 404)，apt-get update 会返回非零；但主源「命中」即可用，不该因此中断安装。
sudo apt-get update || echo "    [提示] apt update 有部分源失败(通常是失效的 backports，可忽略)，继续。"
sudo apt-get install -y \
    curl ca-certificates gnupg xz-utils \
    redis-server \
    python3 python3-pip \
    libgtk-3-0 \
    libgstreamer1.0-0 libgstreamer-plugins-base1.0-0 \
    libgstrtspserver-1.0-0 \
    gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav gstreamer1.0-tools

# 硬件 H264/H265 编码（RTSP 推流 rtsp_encoder=hw）需 Rockchip MPP 的 gst 插件，
# 通常来自板子 BSP/Armbian 源，名字可能是 gstreamer1.0-rockchip。装不上不影响软件编码。
sudo apt-get install -y gstreamer1.0-rockchip 2>/dev/null \
    || echo "    [跳过] gstreamer1.0-rockchip 未找到（仅影响 RTSP 硬件编码，软件编码 x264 仍可用）"

# ---- [2] APT 编译依赖（可选）----
if [ "$WANT_BUILD" = true ]; then
    echo ">>> [2/5] 安装 APT 编译依赖（板端从源码编译主程序）..."
    sudo apt-get install -y \
        build-essential cmake pkg-config \
        libopencv-dev libgtk-3-dev libhiredis-dev \
        libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstrtspserver-1.0-dev \
        libblas-dev liblapack-dev
    echo "    注: RKNN/RGA 头文件来自 Rockchip BSP；运行库已在 rk3588_yolo/test1/libs。"
else
    echo ">>> [2/5] 跳过编译依赖（如需板端编译主程序，加 --build 重跑）"
fi

# ---- [3] Node.js（前端构建需要）----
echo ">>> [3/5] 安装 Node.js + npm (前端构建需 Node 18+)..."
NODE_OK=false
if command -v node >/dev/null 2>&1; then
    NODE_MAJOR=$(node -v 2>/dev/null | sed -n 's/v\([0-9]*\).*/\1/p')
    [ -n "$NODE_MAJOR" ] && [ "$NODE_MAJOR" -ge 18 ] && NODE_OK=true
fi
if [ "$NODE_OK" = true ]; then
    echo "    已有 Node $(node -v)，跳过"
else
    # 预编译 tarball 自带 npm、单包下载，绕开 NodeSource(国外) 和发行版 npm 的多包依赖地狱。
    # 可用 NODE_VERSION=vX.Y.Z 覆盖版本；RK3588 = aarch64 = arm64。
    NODE_VERSION="${NODE_VERSION:-v20.18.0}"
    PKG="node-${NODE_VERSION}-linux-arm64"
    installed=false
    for url in \
        "https://registry.npmmirror.com/-/binary/node/${NODE_VERSION}/${PKG}.tar.xz" \
        "https://mirrors.tuna.tsinghua.edu.cn/nodejs-release/${NODE_VERSION}/${PKG}.tar.xz" \
        "https://nodejs.org/dist/${NODE_VERSION}/${PKG}.tar.xz"; do
        echo "    下载 Node: $url"
        if curl -fSL --connect-timeout 15 -o "/tmp/${PKG}.tar.xz" "$url"; then
            if sudo tar -xJf "/tmp/${PKG}.tar.xz" -C /usr/local --strip-components=1; then
                rm -f "/tmp/${PKG}.tar.xz"; installed=true; break
            fi
        fi
        echo "    该源失败，换下一个..."
    done
    if [ "$installed" != true ]; then
        echo "    [回退] tarball 全部失败，尝试 NodeSource（国外源，可能慢/超时）..."
        (curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash - && sudo apt-get install -y nodejs) \
            || echo "    [警告] Node 自动安装失败，请手动安装 Node 18+（见文件头注释）"
    fi
    hash -r 2>/dev/null || true
fi

# 校验 npm（tarball/NodeSource 都自带 npm；不再走发行版 npm 多包依赖，避免逐包下载被重置）
if command -v npm >/dev/null 2>&1; then
    echo "    npm 已就绪: $(npm -v)"
else
    echo "    [警告] 未检测到 npm。若刚用 tarball 装的，确认 /usr/local/bin 在 PATH（执行 hash -r 后重试）。"
fi

# ---- [4] 启动 Redis ----
echo ">>> [4/5] 启用并启动 Redis（两个微服务用它作消息队列）..."
sudo systemctl enable redis-server || true
sudo systemctl restart redis-server || sudo service redis-server restart || true

# ---- [5] pip 依赖 ----
echo ">>> [5/5] 安装 pip 依赖（控制台后端 + 两个微服务）..."
python3 -m pip install --upgrade pip -q || true
for req in $(find "$PROJ" -name requirements.txt 2>/dev/null \
             | grep -vE "node_modules|/tests?/|/test_"); do
    echo "    -> $req"
    python3 -m pip install -r "$req" || {
        echo "    [警告] $req 安装失败。"
        echo "          若卡在 uvicorn[standard] 的 watchfiles/uvloop（aarch64 偶发需编译），"
        echo "          可改装精简版：python3 -m pip install uvicorn fastapi"
    }
done

echo ""
echo "[OK] 依赖安装完成。"
echo "  • Node: $(command -v node >/dev/null 2>&1 && node -v || echo '未安装(请检查上面的日志)')"
echo "  • npm:  $(command -v npm  >/dev/null 2>&1 && npm -v  || echo '未安装')"
echo "  • 控制台部署： cd web_console && bash install.sh"
echo "  • 主程序 + 两个微服务部署： 见 rk3588_yolo build.sh 产物里的 deploy.sh"
