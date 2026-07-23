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
