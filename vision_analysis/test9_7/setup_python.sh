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
