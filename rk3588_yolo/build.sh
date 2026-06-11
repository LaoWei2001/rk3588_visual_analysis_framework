#!/bin/bash
# ============================================================
# 统一构建与打包脚本 (支持: RK3588板端原生编译 / X86_Docker交叉编译)
# 用法： ./build.sh <输出目录名> [选项]
#   <输出目录名>                          # [必填] 编译产物文件夹名(建于项目目录下), 如 dist / release_v8
#   ./build.sh dist                       # 自动根据当前 CPU 架构推断模式, 产物输出到 ./dist
#   ./build.sh release --mode onboard     # 强制板端原生编译, 产物输出到 ./release
#   ./build.sh out --mode docker          # 强制 Docker 交叉编译
#   ./build.sh out --no-strip             # 禁用 strip (保留调试信息)
#   ./build.sh out --no-bundle-libs       # 不打包依赖动态库 (仅原生编译有效)
#   ./build.sh out --image <name>         # 指定交叉编译 Docker 镜像名
# ============================================================

set -e

PROJECT_DIR=$(cd "$(dirname "$0")"; pwd)
REPO_ROOT=$(dirname "$PROJECT_DIR")
TARGET="rk3588_yolo"

# --- 默认配置 ---
OUT_NAME=""          # [必填] 输出目录名, 由命令行传入 (替代原先写死的 dist)
MODE="auto"
DO_STRIP=true
BUNDLE_LIBS=true
IMAGE_NAME="rk3588_builder:2026_4_30"

# 需要打包的 Python 微服务列表（相对路径:目标名, 路径相对于项目根 common/）
PYTHON_SERVICES=(
    "service/model_update:model_update"
    "service/upload:upload"
)

# --- 命令行参数解析 ---
usage() {
    echo "用法: ./build.sh <输出目录名> [--mode onboard|docker] [--no-strip] [--no-bundle-libs] [--image <name>]"
    echo "  <输出目录名>  [必填] 编译产物文件夹名 (创建于 $PROJECT_DIR 下), 如 dist / release_v8"
}
while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode)            MODE="$2"; shift 2 ;;
        --no-strip)        DO_STRIP=false; shift ;;
        --no-bundle-libs)  BUNDLE_LIBS=false; shift ;;
        --image)           IMAGE_NAME="$2"; shift 2 ;;
        -h|--help)         usage; exit 0 ;;
        --*)               echo "[ERROR] 未知选项: $1"; usage; exit 1 ;;
        *)
            if [ -z "$OUT_NAME" ]; then
                OUT_NAME="$1"
            else
                echo "[ERROR] 多余的参数: $1 (输出目录名只需一个)"; usage; exit 1
            fi
            shift ;;
    esac
done

# --- 校验必填的输出目录名 ---
if [ -z "$OUT_NAME" ]; then
    echo "[ERROR] 必须指定输出目录名 (必填参数)。"
    usage
    exit 1
fi
# 安全校验: 只允许单层目录名, 禁止 '/' 与 '.'/'..' (避免下面的 rm -rf 误删父目录等)
case "$OUT_NAME" in
    */*|.|..) echo "[ERROR] 输出目录名只能是单层名字, 不能含 '/' 或为 '.' / '..': $OUT_NAME"; exit 1 ;;
esac

DIST_DIR="$PROJECT_DIR/$OUT_NAME"

# --- 自动推断模式 ---
if [ "$MODE" = "auto" ]; then
    ARCH=$(uname -m)
    if [[ "$ARCH" == "aarch64" || "$ARCH" == "armv7l" ]]; then
        MODE="onboard"
    else
        MODE="docker"
    fi
    echo "[Sys] 自动检测架构: $ARCH -> 选定编译模式: $MODE"
fi

echo "========================================================"
echo "  RK3588 视觉系统统一构建脚本"
echo "  编译模式 : $MODE"
echo "  项目目录 : $PROJECT_DIR"
echo "  输出目录 : $DIST_DIR"
echo "  Strip    : $DO_STRIP"
if [ "$MODE" = "docker" ]; then
    echo "  Docker镜像: $IMAGE_NAME"
else
    echo "  打包动态库: $BUNDLE_LIBS"
fi
echo "========================================================"

# --- 编译前端准备 ---
cd "$PROJECT_DIR"
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR/libs"

# ============================================================
# 分支 1: Docker 交叉编译 (运行于 x86_64)
# ============================================================
if [ "$MODE" = "docker" ]; then
    if ! docker image inspect "$IMAGE_NAME" > /dev/null 2>&1; then
        echo "[ERROR] Docker 镜像 '$IMAGE_NAME' 不存在，请先构建镜像。"
        exit 1
    fi

    echo ">>> [1/4] 启动 Docker 进行交叉编译..."
    docker run --rm -i \
        -v "$PROJECT_DIR:/workspace" \
        -e DO_STRIP="$DO_STRIP" \
        -e DIST_NAME="$OUT_NAME" \
        -e HOST_UID=$(id -u) \
        -e HOST_GID=$(id -g) \
        "$IMAGE_NAME" bash << 'DOCKER_CMD'
set -e
TARGET="rk3588_yolo"
DIST="${DIST_NAME:-dist}"   # 由宿主机 -e DIST_NAME 传入(单引号 heredoc, 在容器内展开)

echo "  [cmake] 配置交叉编译工具链..."
cat > /workspace/cross.cmake << 'CROSS_EOF'
set(CMAKE_SYSTEM_NAME    Linux)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(TOOLCHAIN_PREFIX "aarch64-linux-gnu")
set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_STRIP        ${TOOLCHAIN_PREFIX}-strip)
set(CMAKE_AR           ${TOOLCHAIN_PREFIX}-ar)

set(CMAKE_SYSROOT      /sysroot)
set(CMAKE_FIND_ROOT_PATH /sysroot)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(ENV{PKG_CONFIG_DIR} "")
set(ENV{PKG_CONFIG_LIBDIR} "/sysroot/usr/lib/pkgconfig:/sysroot/usr/share/pkgconfig:/sysroot/usr/lib/aarch64-linux-gnu/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "/sysroot")
CROSS_EOF
rm -rf build && mkdir -p build && cd build
cmake .. -DCROSS_CMAKE_FILE=/workspace/cross.cmake -DCMAKE_BUILD_TYPE=Release
echo "  [make] 开始编译..."
make -j$(nproc)
cd ..

cp build/$TARGET $DIST/

if [ "$DO_STRIP" = "true" ]; then
    echo "  [strip] 压缩二进制文件体积..."
    aarch64-linux-gnu-strip "$DIST/$TARGET"
fi

echo "  [readelf] 提取系统动态库依赖..."
libs=$(aarch64-linux-gnu-readelf -d build/$TARGET | grep "NEEDED" | sed -r 's/.*\[(.*)\].*/\1/' | grep -vE "^(ld-linux|libc\.so|libm\.so|libdl\.so|librt\.so|libpthread\.so|libstdc\+\+\.so)")
for lib in $libs; do
    lib_path=$(find /sysroot -name "$lib" -print -quit 2>/dev/null)
    if [ -n "$lib_path" ]; then
        cp -L "$lib_path" $DIST/libs/
    fi
done

chown -R $HOST_UID:$HOST_GID $DIST build
echo "  Docker 构建流完成。"
DOCKER_CMD

# ============================================================
# 分支 2: 板端原生编译 (运行于 RK3588 AArch64)
# ============================================================
elif [ "$MODE" = "onboard" ]; then
    echo ">>> [1/4] 开始板端原生编译..."
    rm -rf build && mkdir -p build && cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release
    make -j$(nproc)
    cd "$PROJECT_DIR"
    
    cp "build/$TARGET" "$DIST_DIR/"
    cp "build/$TARGET" "$PROJECT_DIR/$TARGET"

    if [ "$DO_STRIP" = "true" ]; then
        echo "  [strip] 压缩二进制文件体积..."
        strip "$DIST_DIR/$TARGET"
    fi
    
    if [ "$BUNDLE_LIBS" = "true" ]; then
        echo "  [readelf] 提取动态库依赖..."
        libs=$(readelf -d "build/$TARGET" | grep "NEEDED" | sed -r 's/.*\[(.*)\].*/\1/' | grep -vE "^(ld-linux|libc\.so|libm\.so|libdl\.so|librt\.so|libpthread\.so|libstdc\+\+\.so)")
        for lib in $libs; do
            lib_path=$(ldconfig -p 2>/dev/null | grep "^\s*${lib}" | awk '{print $NF}' | head -1)
            [ -z "$lib_path" ] && lib_path=$(find /usr/lib /usr/local/lib -name "$lib" -print -quit 2>/dev/null)
            if [ -n "$lib_path" ]; then
                cp -L "$lib_path" "$DIST_DIR/libs/"
            fi
        done
    fi
else
    echo "[ERROR] 未知的编译模式: $MODE"; exit 1
fi

# 检查编译产物
if [ ! -f "$DIST_DIR/$TARGET" ]; then
    echo "[ERROR] 构建失败，未找到产物 $TARGET。"
    exit 1
fi

# --- 拷贝通用资产与 Python 源码 ---
echo ""
echo ">>> [2/4] 拷贝通用资产与 Python 微服务..."
if [ -d "$PROJECT_DIR/assets" ]; then
    cp -rp "$PROJECT_DIR/assets" "$DIST_DIR/"
fi

# 逻辑参数清单（供 Web 控制台动态渲染各 logic 的可调参数；后端读 app 根目录下的 logics.json）
if [ -f "$PROJECT_DIR/src/logic/logics.json" ]; then
    cp "$PROJECT_DIR/src/logic/logics.json" "$DIST_DIR/logics.json"
    echo "  打包: src/logic/logics.json  ->  logics.json"
fi

mkdir -p "$DIST_DIR/services"
for entry in "${PYTHON_SERVICES[@]}"; do
    SRC_REL="${entry%%:*}"
    DST_NAME="${entry##*:}"
    SRC_PATH="$REPO_ROOT/$SRC_REL"
    DST_PATH="$DIST_DIR/services/$DST_NAME"

    if [ ! -d "$SRC_PATH" ]; then
        echo "  [WARN] 跳过 $DST_NAME: 源目录 $SRC_PATH 不存在"
        continue
    fi

    mkdir -p "$DST_PATH"
    rsync -a \
        --exclude='__pycache__' \
        --exclude='*.pyc' \
        --exclude='logs/' \
        --exclude='.git' \
        --exclude='*.log' \
        "$SRC_PATH/" "$DST_PATH/"
    echo "  打包: $SRC_REL  ->  services/$DST_NAME"
done

# --- 生成运行时脚本 ---
echo ""
echo ">>> [3/4] 生成运行时与部署脚本 (systemd 架构)..."

# ---------- run.sh (前台运行模式) ----------
cat > "$DIST_DIR/run.sh" << 'RUN_EOF'
#!/bin/bash
if [ -z "$1" ]; then
    echo "用法: bash run.sh <配置文件路径>"
    echo "示例: bash run.sh ./assets/config_mux16.json"
    exit 1
fi
CONFIG_PATH="$1"

if [ ! -f "$CONFIG_PATH" ]; then
    echo "[ERROR] 配置文件不存在: $CONFIG_PATH"
    exit 1
fi

# 前台调试模式，开启显示
echo ">>> 修改配置: enable_display = 1 (开启前台显示输出)..."
sed -i 's/"enable_display"[ \t]*:[ \t]*[a-zA-Z0-9_"]*/"enable_display": 1/' "$CONFIG_PATH"

ABS_PATH=$(cd "$(dirname "$0")"; pwd)
export LD_LIBRARY_PATH="$ABS_PATH/libs:$LD_LIBRARY_PATH"
export ASSETS_DIR="$ABS_PATH/assets"
exec "$ABS_PATH/rk3588_yolo" "$CONFIG_PATH"
RUN_EOF
chmod +x "$DIST_DIR/run.sh"

# ---------- setup_python.sh ----------
cat > "$DIST_DIR/setup_python.sh" << 'SETUP_EOF'
#!/bin/bash
set -e
ABS_PATH=$(cd "$(dirname "$0")"; pwd)
echo ">>> 初始化 Python 环境 (使用系统级 pip)..."
python3 -m pip install --upgrade pip -q || true
for req in $(find "$ABS_PATH/services" -name requirements.txt); do
    if [ -f "$req" ]; then
        echo ">>> 安装 $(basename "$(dirname "$req")") 依赖..."
        python3 -m pip install -r "$req"
    fi
done
echo "[OK] 环境安装完成。"
SETUP_EOF
chmod +x "$DIST_DIR/setup_python.sh"

# ---------- deploy.sh (交互式一键部署到 systemd) ----------
cat > "$DIST_DIR/deploy.sh" << 'DEPLOY_EOF'
#!/bin/bash
# 注意: 交互式脚本为了更好的用户体验，未设置全局 set -e

ABS_PATH=$(cd "$(dirname "$0")"; pwd)

if [ -z "$1" ]; then
    echo "用法: bash deploy.sh <配置文件路径>"
    echo "示例: bash deploy.sh ./assets/config_mux16.json"
    exit 1
fi
CONFIG_PATH="$1"

echo "============================================================"
echo "  RK3588 视觉系统 — systemd 交互式部署"
echo "  部署目录: $ABS_PATH"
echo "  配置文件: $CONFIG_PATH"
echo "============================================================"

if [ ! -f "$CONFIG_PATH" ]; then
    echo "[ERROR] 配置文件不存在: $CONFIG_PATH"
    exit 1
fi

# ---- Step 0: 关闭显示输出 (enable_display=0) ----
echo ""
echo ">>> [0/4] 修改配置: enable_display = 0 (后台部署模式)..."
sed -i 's/"enable_display"[ \t]*:[ \t]*[a-zA-Z0-9_"]*/"enable_display": 0/' "$CONFIG_PATH"
echo "  [OK] 已关闭显示输出"

# ---- Step 1: 配置 journald 为内存模式 (不写磁盘) ----
echo ""
echo ">>> [1/4] 配置 journald 为 volatile 模式 (日志仅存内存)..."
sudo mkdir -p /etc/systemd/journald.conf.d
cat << 'JOURNAL_EOF' | sudo tee /etc/systemd/journald.conf.d/volatile.conf > /dev/null
[Journal]
Storage=volatile
RuntimeMaxUse=32M
ForwardToSyslog=no
JOURNAL_EOF
sudo systemctl restart systemd-journald
echo "  [OK] journald 日志仅存内存, 不占用磁盘空间"

# ---- Step 2: 生成并注册 systemd 服务 ----
echo ""
echo ">>> [2/4] 生成并注册 systemd 服务文件..."

# vision_app.service (修改核心：直接运行二进制，绕开 run.sh)
cat << SERVICE_EOF | sudo tee /etc/systemd/system/vision_app.service > /dev/null
[Unit]
Description=RK3588 Vision AI Detection Service
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=$ABS_PATH
Environment=ASSETS_DIR=$ABS_PATH/assets
Environment="LD_LIBRARY_PATH=$ABS_PATH/libs:/usr/lib:/usr/local/lib"
ExecStart=$ABS_PATH/rk3588_yolo $CONFIG_PATH
Restart=always
RestartSec=5
User=root

[Install]
WantedBy=multi-user.target
SERVICE_EOF
echo "  [OK] vision_app.service"

# ota_agent.service
cat << SERVICE_EOF | sudo tee /etc/systemd/system/ota_agent.service > /dev/null
[Unit]
Description=Edge Box OTA Agent
After=network.target

[Service]
Type=simple
WorkingDirectory=$ABS_PATH/services/model_update
Environment=ASSETS_DIR=$ABS_PATH/assets
ExecStart=/usr/bin/python3 -u $ABS_PATH/services/model_update/ota_agent.py
Restart=always
RestartSec=3
User=root

[Install]
WantedBy=multi-user.target
SERVICE_EOF
echo "  [OK] ota_agent.service"

# unified_upload.service
cat << SERVICE_EOF | sudo tee /etc/systemd/system/unified_upload.service > /dev/null
[Unit]
Description=RK3588 Unified Upload Service
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=$ABS_PATH/services/upload
ExecStart=/usr/bin/python3 -u main.py
Restart=always
RestartSec=5
User=root

[Install]
WantedBy=multi-user.target
SERVICE_EOF
echo "  [OK] unified_upload.service"

# ---- Step 3: 交互式选择启动服务 ----
echo ""
echo ">>> [3/4] 启动服务 (交互式选择)..."
sudo systemctl daemon-reload

AVAILABLE_SERVICES=("vision_app" "ota_agent" "unified_upload")

echo "============================================================"
echo " 配置文件已生成完毕，请选择需要启用并启动的服务:"
for i in "${!AVAILABLE_SERVICES[@]}"; do
    echo "  [$((i+1))] ${AVAILABLE_SERVICES[$i]}"
done
echo ""
echo "  [a] 全部启动"
echo "  [q] 暂不启动，退出脚本"
echo "============================================================"

read -p "请输入要启动的服务编号(多个用空格隔开，如 '1 2'), 'a' 全选, 'q' 退出: " choice

if [[ -z "$choice" || "$choice" == "q" || "$choice" == "Q" ]]; then
    echo "已取消启动操作。服务已注册但未运行，您稍后可以手动启动。"
    exit 0
fi

TARGET_SERVICES=()
if [[ "$choice" == "a" || "$choice" == "A" ]]; then
    TARGET_SERVICES=("${AVAILABLE_SERVICES[@]}")
else
    for idx in $choice; do
        if [[ "$idx" =~ ^[0-9]+$ ]] && [ "$idx" -ge 1 ] && [ "$idx" -le "${#AVAILABLE_SERVICES[@]}" ]; then
            TARGET_SERVICES+=("${AVAILABLE_SERVICES[$((idx-1))]}")
        else
            echo " [WARN] 无效的选择: $idx, 已跳过"
        fi
    done
fi

if [ ${#TARGET_SERVICES[@]} -eq 0 ]; then
    echo "未选择任何有效的服务，退出。"
    exit 0
fi

echo ""
echo ">>> 开始启用并启动选择的服务..."
FAILED=()
for svc in "${TARGET_SERVICES[@]}"; do
    sudo systemctl enable "$svc" --now
    sleep 1
    if [ "$(systemctl is-active "$svc")" = "active" ]; then
        echo "  [OK] $svc 已启动"
    else
        echo "  [FAIL] $svc 启动失败!"
        FAILED+=("$svc")
    fi
done

echo ""
echo "============================================================"
if [ ${#FAILED[@]} -eq 0 ]; then
    echo "  部署成功! 已选服务运行正常。查看实时输出命令:"
else
    echo "  部分失败, 排查命令: sudo systemctl status ${FAILED[*]}"
    echo "  成功启动的服务查看实时输出命令:"
fi
echo ""
for svc in "${TARGET_SERVICES[@]}"; do
    # 如果启动失败就不打印其 journalctl 命令
    if [[ ! " ${FAILED[*]} " =~ " ${svc} " ]]; then
        echo "    journalctl -u $svc -f"
    fi
done
echo ""
echo "  停止/重启: sudo systemctl stop/restart <服务名>"
echo "============================================================"
DEPLOY_EOF
chmod +x "$DIST_DIR/deploy.sh"

# ---------- stop.sh (交互式关闭服务) ----------
cat > "$DIST_DIR/stop.sh" << 'STOP_EOF'
#!/bin/bash
# 注意: 交互式脚本不建议全局使用 set -e，避免用户输错或者检测未运行直接退出脚本

SERVICES=("vision_app" "ota_agent" "unified_upload")
ACTIVE_SERVICES=()

echo ">>> 正在检测服务状态..."
for svc in "${SERVICES[@]}"; do
    # 如果服务已经 enable 或者正在运行，则加入候选列表
    if systemctl is-enabled "$svc" &>/dev/null || systemctl is-active "$svc" &>/dev/null; then
        ACTIVE_SERVICES+=("$svc")
    fi
done

if [ ${#ACTIVE_SERVICES[@]} -eq 0 ]; then
    echo ">>> 当前没有发现由本系统配置的运行中/已启用服务。"
    exit 0
fi

echo "============================================================"
echo " 发现以下服务正在运行或已启用:"
for i in "${!ACTIVE_SERVICES[@]}"; do
    _curr_svc="${ACTIVE_SERVICES[$i]}"
    _status=$(systemctl is-active "$_curr_svc" 2>/dev/null || echo "inactive")
    echo "  [$((i+1))] $_curr_svc (当前状态: $_status)"
done
echo ""
echo "  [a] 全部关闭并禁用"
echo "  [q] 取消并退出"
echo "============================================================"

read -p "请输入要关闭的服务编号(多个用空格隔开，如 '1 2'), 'a' 全选, 'q' 退出: " choice

if [[ -z "$choice" || "$choice" == "q" || "$choice" == "Q" ]]; then
    echo "已取消操作。"
    exit 0
fi

TARGET_SERVICES=()
if [[ "$choice" == "a" || "$choice" == "A" ]]; then
    TARGET_SERVICES=("${ACTIVE_SERVICES[@]}")
else
    for idx in $choice; do
        if [[ "$idx" =~ ^[0-9]+$ ]] && [ "$idx" -ge 1 ] && [ "$idx" -le "${#ACTIVE_SERVICES[@]}" ]; then
            TARGET_SERVICES+=("${ACTIVE_SERVICES[$((idx-1))]}")
        else
            echo " [WARN] 无效的选择: $idx, 已跳过"
        fi
    done
fi

if [ ${#TARGET_SERVICES[@]} -eq 0 ]; then
    echo "未选择任何要关闭的服务，退出。"
    exit 0
fi

echo ""
echo ">>> 开始停止并禁用选择的服务..."
for svc in "${TARGET_SERVICES[@]}"; do
    sudo systemctl stop "$svc" 2>/dev/null || true
    sudo systemctl disable "$svc" 2>/dev/null || true
    echo "  [OK] $svc 已关闭并禁用"
done

sudo systemctl daemon-reload
echo ""
echo "操作完成。"
STOP_EOF
chmod +x "$DIST_DIR/stop.sh"

echo ""
echo "========================================================"
echo "  构建完成！ ($MODE 模式)"
echo "  发布目录: $DIST_DIR"
echo "========================================================"