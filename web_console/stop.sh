#!/bin/bash
# 关闭整个 Web 控制台应用（systemd 服务 rk3588-console）。
#
# 说明：本脚本只停"控制台"本身（网页后端 + 界面）。控制台管理的
#   - 推理程序进程（你在「程序管理」里启动的 App）
#   - 后台微服务（ota_agent / unified_upload）
# 是各自独立的 systemd 单元 / 进程，默认不受影响。若要一并停，见末尾提示。
#
# 用法：  bash stop.sh            # 停止控制台（保留开机自启）
#         bash stop.sh --disable  # 停止并禁用开机自启

SERVICE=rk3588-console

echo ">>> 停止 Web 控制台（$SERVICE）..."
sudo systemctl stop "$SERVICE" 2>/dev/null || echo "  ($SERVICE 未在运行或未安装)"

if [ "$1" == "--disable" ]; then
    sudo systemctl disable "$SERVICE" 2>/dev/null || true
    echo "  已禁用开机自启。"
fi

echo ""
echo ">>> 当前状态："
systemctl --no-pager status "$SERVICE" 2>/dev/null | head -4 || true

echo ""
echo "✓ Web 控制台已停止。重新启动： sudo systemctl start $SERVICE"
echo ""
echo "  以下不受本脚本影响（如需停止请手动）："
echo "    推理程序：     在网页「程序管理」点停止，或  sudo systemctl stop vision_app"
echo "    后台微服务：   sudo systemctl stop ota_agent unified_upload"
