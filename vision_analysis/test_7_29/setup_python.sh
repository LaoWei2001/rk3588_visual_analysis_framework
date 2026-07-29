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
