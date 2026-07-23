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
