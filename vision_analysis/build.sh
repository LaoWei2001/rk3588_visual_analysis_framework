#!/bin/bash
# ============================================================
# 统一构建与打包脚本 (自动识别架构: aarch64->板端原生编译, x86_64->Docker交叉编译)
# 用法： ./build.sh <输出目录名> [选项]
#   <输出目录名>                          # [必填] 编译产物文件夹名(建于项目目录下), 如 dist / release_v8
#   ./build.sh --debug                    # [快速调试] 只编译出可执行文件(Debug构建), 不打包, 产物直接放在 build.sh 同级目录
#   ./build.sh dist                       # 自动识别架构选择编译方式, 产物输出到 ./dist
#   ./build.sh out --no-strip             # 禁用 strip (保留调试信息)
#   ./build.sh out --no-bundle-libs       # 不打包依赖动态库
#   ./build.sh out --image <name>         # 指定交叉编译 Docker 镜像名
#   ./build.sh out --clean                # 清除已有 build/ 后进行全量编译
# ============================================================

set -e

PROJECT_DIR=$(cd "$(dirname "$0")"; pwd)
REPO_ROOT=$(dirname "$PROJECT_DIR")
TARGET="vision_analysis"

# --- 项目锁定的 RKNN 用户态 Runtime ---
# 正式编译与打包都只使用此目录，禁止从构建机的 ldconfig/sysroot 随机选择同名库。
RKNN_VENDOR_VERSION="2.4.2a2"
RKNN_VENDOR_ROOT="$PROJECT_DIR/vendor/rknn/$RKNN_VENDOR_VERSION"
RKNN_VENDOR_LIB="$RKNN_VENDOR_ROOT/lib/aarch64/librknnrt.so"
RKNN_VENDOR_MANIFEST="$RKNN_VENDOR_ROOT/RUNTIME_MANIFEST.txt"
RKNN_VENDOR_SHA256="bf50d51705ae433013927a13520ae781b534fdb1481c47bdddbc726f63ed4970"

verify_vendor_rknn() {
    local version_line

    if [ ! -f "$RKNN_VENDOR_ROOT/SHA256SUMS" ]; then
        echo "[ERROR] 缺少 RKNN 校验清单: $RKNN_VENDOR_ROOT/SHA256SUMS"
        exit 1
    fi
    if ! (cd "$RKNN_VENDOR_ROOT" && sha256sum -c SHA256SUMS); then
        echo "[ERROR] 项目锁定的 RKNN SDK 校验失败，拒绝使用系统库继续构建。"
        exit 1
    fi
    if ! LC_ALL=C readelf -h "$RKNN_VENDOR_LIB" | grep -q 'Machine:.*AArch64'; then
        echo "[ERROR] RKNN Runtime 不是 AArch64 ELF: $RKNN_VENDOR_LIB"
        exit 1
    fi
    if ! LC_ALL=C readelf -d "$RKNN_VENDOR_LIB" | grep -q 'Library soname: \[librknnrt.so\]'; then
        echo "[ERROR] RKNN Runtime SONAME 不是 librknnrt.so: $RKNN_VENDOR_LIB"
        exit 1
    fi

    version_line=$(strings "$RKNN_VENDOR_LIB" | grep -F -m1 'librknnrt version:' || true)
    case "$version_line" in
        *"librknnrt version: $RKNN_VENDOR_VERSION "*) ;;
        *)
            echo "[ERROR] RKNN Runtime 版本不符合锁定值 $RKNN_VENDOR_VERSION"
            echo "        检测结果: ${version_line:-未找到版本信息}"
            exit 1
            ;;
    esac

    echo "  RKNN Runtime: $version_line"
    echo "  RKNN SHA-256: $RKNN_VENDOR_SHA256"
}

bundle_vendor_rknn() {
    local destination="$1"
    local packaged_sha256

    mkdir -p "$destination"
    cp -L "$RKNN_VENDOR_LIB" "$destination/librknnrt.so"
    cp "$RKNN_VENDOR_MANIFEST" "$destination/librknnrt.manifest"

    packaged_sha256=$(sha256sum "$destination/librknnrt.so" | awk '{print $1}')
    if [ "$packaged_sha256" != "$RKNN_VENDOR_SHA256" ]; then
        echo "[ERROR] 打包后的 librknnrt.so 校验失败: $packaged_sha256"
        exit 1
    fi
    echo "  固定: RKNN Runtime $RKNN_VENDOR_VERSION -> libs/librknnrt.so"
}

# --- 默认配置 ---
OUT_NAME=""          # 输出目录名, 由命令行传入 (--debug 模式无需指定)
MODE=""              # 编译模式, 启动时按 CPU 架构自动识别 (onboard / docker)
DO_STRIP=true
BUNDLE_LIBS=true
BUILD_TYPE="Release" # CMake 构建类型; --debug 模式改为 Debug
DEBUG_ONLY=false     # true=只编译可执行文件, 跳过打包(由命令行的 --debug 选项触发)
CLEAN_BUILD=false    # true=编译前清除 build/，默认复用构建缓存进行增量编译
IMAGE_NAME="rk3588_builder:2026_4_30"

# 需要打包的 Python 微服务列表（相对路径:目标名, 路径相对于项目根 common/）
PYTHON_SERVICES=(
    "service/model_update:model_update"
    "service/upload:upload"
)

# --- 命令行参数解析 ---
usage() {
    echo "用法: ./build.sh <输出目录名> [--debug] [--clean] [--no-strip] [--no-bundle-libs] [--image <name>]"
    echo "  (编译方式按 CPU 架构自动识别: aarch64->板端原生, x86_64->Docker交叉编译)"
    echo "  <输出目录名>  编译产物文件夹名 (创建于 $PROJECT_DIR 下), 如 dist / release_v8"
    echo "  --debug       只编译可执行文件(Debug构建), 不打包, 产物直接放在 build.sh 同级目录"
    echo "  --clean       编译前清除已有 build/；默认保留 build/ 并进行增量编译"
}
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-strip)        DO_STRIP=false; shift ;;
        --no-bundle-libs)  BUNDLE_LIBS=false; shift ;;
        --image)           IMAGE_NAME="$2"; shift 2 ;;
        -h|--help)         usage; exit 0 ;;
        --debug)           DEBUG_ONLY=true; BUILD_TYPE=Debug; DO_STRIP=false; BUNDLE_LIBS=false; shift ;;
        --clean)           CLEAN_BUILD=true; shift ;;
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

# --- 确定产物输出位置 ---
if [ "$DEBUG_ONLY" = "true" ]; then
    # debug: 不单独建文件夹, 可执行文件直接放在 build.sh 同级目录(项目根)
    DIST_DIR="$PROJECT_DIR"
    OUT_NAME="."          # 传给 Docker 容器: 直接拷到 /workspace 根目录
else
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
fi

# --- 按 CPU 架构自动识别编译方式 (无需手动指定) ---
ARCH=$(uname -m)
if [[ "$ARCH" == "aarch64" || "$ARCH" == "armv7l" ]]; then
    MODE="onboard"
else
    MODE="docker"
fi
echo "[Sys] 自动检测架构: $ARCH -> 编译方式: $MODE"

echo "========================================================"
echo "  RK3588 视觉系统统一构建脚本"
echo "  编译模式 : $MODE"
echo "  构建类型 : $BUILD_TYPE"
if [ "$DEBUG_ONLY" = "true" ]; then
echo "  调试模式 : 仅编译可执行文件 (跳过打包)"
fi
echo "  项目目录 : $PROJECT_DIR"
echo "  输出目录 : $DIST_DIR"
echo "  清理构建 : $CLEAN_BUILD"
echo "  Strip    : $DO_STRIP"
echo "  RKNN     : $RKNN_VENDOR_VERSION (项目锁定)"
if [ "$MODE" = "docker" ]; then
    echo "  Docker镜像: $IMAGE_NAME"
else
    echo "  打包动态库: $BUNDLE_LIBS"
fi
echo "========================================================"

echo ">>> [preflight] 校验项目锁定的 RKNN SDK..."
verify_vendor_rknn

# --- 编译前准备 ---
cd "$PROJECT_DIR"
if [ "$CLEAN_BUILD" = "true" ]; then
    echo ">>> [clean] 清除旧构建目录: $PROJECT_DIR/build"
    rm -rf "$PROJECT_DIR/build"
else
    echo ">>> [incremental] 保留 build/，复用已有构建缓存"
fi
if [ "$DEBUG_ONLY" = "true" ]; then
    : # debug: 产物直接放项目根目录, 不清空、不另建目录
else
    rm -rf "$DIST_DIR"
    if [ "$BUNDLE_LIBS" = "true" ]; then
        mkdir -p "$DIST_DIR/libs"
    else
        mkdir -p "$DIST_DIR"
    fi
fi

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
        -e BUNDLE_LIBS="$BUNDLE_LIBS" \
        -e BUILD_TYPE="$BUILD_TYPE" \
        -e RKNN_VENDOR_VERSION="$RKNN_VENDOR_VERSION" \
        -e DIST_NAME="$OUT_NAME" \
        -e HOST_UID=$(id -u) \
        -e HOST_GID=$(id -g) \
        "$IMAGE_NAME" bash << 'DOCKER_CMD'
set -e
TARGET="vision_analysis"
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
mkdir -p build && cd build
cmake .. \
    -DCROSS_CMAKE_FILE=/workspace/cross.cmake \
    -DCMAKE_BUILD_TYPE=${BUILD_TYPE:-Release} \
    -DRKNN_VENDOR_ROOT=/workspace/vendor/rknn/${RKNN_VENDOR_VERSION}
echo "  [make] 开始编译..."
make -j$(nproc)
cd ..

cp build/$TARGET $DIST/

if [ "$DO_STRIP" = "true" ]; then
    echo "  [strip] 压缩二进制文件体积..."
    aarch64-linux-gnu-strip "$DIST/$TARGET"
fi

if [ "$BUNDLE_LIBS" = "true" ]; then
    echo "  [readelf] 提取系统动态库依赖..."
    libs=$(aarch64-linux-gnu-readelf -d build/$TARGET | grep "NEEDED" | sed -r 's/.*\[(.*)\].*/\1/' | grep -vE "^(ld-linux|libc\.so|libm\.so|libdl\.so|librt\.so|libpthread\.so|libstdc\+\+\.so|librknnrt\.so$)")
    for lib in $libs; do
        lib_path=$(find /sysroot -name "$lib" -print -quit 2>/dev/null)
        if [ -n "$lib_path" ]; then
            cp -L "$lib_path" $DIST/libs/
        fi
    done
fi

chown -R $HOST_UID:$HOST_GID build
chown $HOST_UID:$HOST_GID "$DIST/$TARGET"
if [ "$BUNDLE_LIBS" = "true" ]; then chown -R $HOST_UID:$HOST_GID "$DIST/libs"; fi
echo "  Docker 构建流完成。"
DOCKER_CMD

# ============================================================
# 分支 2: 板端原生编译 (运行于 RK3588 AArch64)
# ============================================================
elif [ "$MODE" = "onboard" ]; then
    echo ">>> [1/4] 开始板端原生编译..."
    mkdir -p build && cd build
    cmake .. \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DRKNN_VENDOR_ROOT="$RKNN_VENDOR_ROOT"
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
        libs=$(readelf -d "build/$TARGET" | grep "NEEDED" | sed -r 's/.*\[(.*)\].*/\1/' | grep -vE "^(ld-linux|libc\.so|libm\.so|libdl\.so|librt\.so|libpthread\.so|libstdc\+\+\.so|librknnrt\.so$)")
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

if ! LC_ALL=C readelf -d "$DIST_DIR/$TARGET" | grep -Fq '[$ORIGIN/libs]'; then
    echo "[ERROR] 构建产物缺少 RUNPATH=\$ORIGIN/libs，无法保证优先加载包内依赖。"
    exit 1
fi

if [ "$BUNDLE_LIBS" = "true" ]; then
    bundle_vendor_rknn "$DIST_DIR/libs"
fi

# --- debug 仅编译模式: 到此结束, 跳过后续所有打包步骤 ---
if [ "$DEBUG_ONLY" = "true" ]; then
    echo ""
    echo "========================================================"
    echo "  [debug] 仅编译完成 — 只生成了可执行文件, 未打包。"
    echo "  可执行文件: $DIST_DIR/$TARGET"
    echo "========================================================"
    exit 0
fi

# --- 拷贝通用资产与 Python 源码 ---
echo ""
echo ">>> [2/4] 拷贝通用资产与 Python 微服务..."
if [ -d "$PROJECT_DIR/assets" ]; then
    cp -rp "$PROJECT_DIR/assets" "$DIST_DIR/"
fi

# 从通道/全局注册宏取 logic ID，并聚合每个模块的 logic.json，
# 生成 Web 控制台兼容的根目录 logics.json。
# 生成器同时校验逻辑身份、参数 Schema，以及 C++ param_* 调用的键和类型。
LOGICS_GENERATOR="$PROJECT_DIR/scripts/generate_logics_catalog.py"
if [ ! -f "$LOGICS_GENERATOR" ]; then
    echo "[ERROR] 缺少逻辑清单生成器: $LOGICS_GENERATOR"
    exit 1
fi
python3 "$LOGICS_GENERATOR" --output "$DIST_DIR/logics.json"
echo "  聚合: channel/global modules + logic.json  ->  logics.json"

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

REPORT_TEMPLATE_GENERATOR="$PROJECT_DIR/scripts/generate_report_templates.py"
python3 "$REPORT_TEMPLATE_GENERATOR" \
    --logic-root "$PROJECT_DIR/src/logic" \
    --app-dir "$PROJECT_DIR/report_templates" \
    --adapter-catalog "$REPO_ROOT/service/upload/adapters/catalog.json" \
    --output "$DIST_DIR/report_templates"
echo "  聚合: Logic/应用上报模板  ->  report_templates/"

# --- 生成运行时脚本 ---
echo ""
echo ">>> [3/4] 生成本地运行与环境初始化脚本..."

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
exec "$ABS_PATH/vision_analysis" "$CONFIG_PATH"
RUN_EOF
chmod +x "$DIST_DIR/run.sh"

# ---------- setup_python.sh ----------
cat > "$DIST_DIR/setup_python.sh" << 'SETUP_EOF'
#!/bin/bash
set -e
ABS_PATH=$(cd "$(dirname "$0")"; pwd)
echo ">>> 初始化 Python 环境 (使用系统级 pip)..."
PIP_ARGS=()
if [ "${OFFLINE:-0}" = "1" ]; then
    PIP_ARGS+=(--no-index)
    echo ">>> 离线模式：只确认 install_deps.sh 已安装的依赖，不访问 PyPI"
else
    python3 -m pip install --upgrade pip -q || true
fi
for req in $(find "$ABS_PATH/services" -name requirements.txt); do
    if [ -f "$req" ]; then
        echo ">>> 安装 $(basename "$(dirname "$req")") 依赖..."
        python3 -m pip install "${PIP_ARGS[@]}" -r "$req"
    fi
done
echo "[OK] 环境安装完成。"
SETUP_EOF
chmod +x "$DIST_DIR/setup_python.sh"

echo ""
echo "========================================================"
echo "  构建完成！ ($MODE 模式)"
echo "  发布目录: $DIST_DIR"
echo "========================================================"
