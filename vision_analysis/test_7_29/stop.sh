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
