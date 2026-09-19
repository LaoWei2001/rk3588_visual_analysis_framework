#!/bin/bash
set -e
ABS_PATH=$(cd "$(dirname "$0")"; pwd)
PYTHON_BIN="/usr/bin/python3"
[ -x "$PYTHON_BIN" ] || { echo "[ERROR] 未找到系统 Python: $PYTHON_BIN"; exit 1; }
if [ "${OFFLINE:-0}" = "1" ]; then
    echo ">>> 离线模式：使用离线依赖包配置好的系统 Python"
    "$PYTHON_BIN" -m pip check
    echo "[OK] Python 环境可用。"
    exit 0
else
    echo ">>> 初始化系统 Python 环境（已满足版本约束的包会跳过）..."
fi
PIP_SYSTEM_ARGS=()
if "$PYTHON_BIN" -m pip help install 2>/dev/null | grep -q -- '--break-system-packages'; then
    PIP_SYSTEM_ARGS+=(--break-system-packages)
fi
for req in $(find "$ABS_PATH/services" -name requirements.txt); do
    if [ -f "$req" ]; then
        echo ">>> 安装 $(basename "$(dirname "$req")") 依赖..."
        PIP_ROOT_USER_ACTION=ignore "$PYTHON_BIN" -m pip install \
            "${PIP_SYSTEM_ARGS[@]}" -r "$req"
    fi
done
"$PYTHON_BIN" -m pip check
echo "[OK] 环境安装完成。"
