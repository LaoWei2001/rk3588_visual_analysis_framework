#!/bin/bash
# ============================================================
# server 队列生产者测试编译脚本
# 用法：
#   ./build.sh          # 编译
#   ./build.sh --clean  # 清除构建目录后重新编译
# ============================================================

set -e

SCRIPT_DIR=$(cd "$(dirname "$0")"; pwd)
BUILD_DIR="$SCRIPT_DIR/build"

# --- 参数解析 ---
DO_CLEAN=false
for arg in "$@"; do
    case "$arg" in
        --clean) DO_CLEAN=true ;;
        --help)
            echo "用法: ./build.sh [--clean]"
            exit 0
            ;;
        *) echo "未知参数: $arg"; exit 1 ;;
    esac
done

if [ "$DO_CLEAN" = true ]; then
    echo ">>> 清除构建目录..."
    rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo ">>> cmake 配置..."
cmake .. -DCMAKE_BUILD_TYPE=Release

echo ">>> 编译..."
make -j$(nproc)

cp "$BUILD_DIR/server_queue_producer_test" "$SCRIPT_DIR/server_queue_producer_test"

echo ""
echo "========================================"
echo "  编译完成: $SCRIPT_DIR/server_queue_producer_test"
echo ""
echo "  用法示例:"
echo "    ./server_queue_producer_test                                   # 自动生成测试图 + 自动定位配置"
echo "    ./server_queue_producer_test --image-draw a.jpg --image-raw b.jpg"
echo "========================================"
