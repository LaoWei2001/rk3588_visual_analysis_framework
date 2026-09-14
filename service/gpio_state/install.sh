#!/usr/bin/env bash
set -euo pipefail

service_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${service_dir}/../.." && pwd)"
gpio_test_dir="${repo_root}/gpio_test"
daemon_build_dir="${service_dir}/build"
# 新设备首次安装时使用低电平作为继电器安全默认值。其他板卡可以在执行安装脚本时
# 通过 RK3588_RELAY_PIN=GPIOx_Yz 覆盖，但升级时绝不覆盖已经保存的实际状态。
relay_safe_pin="${RK3588_RELAY_PIN:-GPIO6_A2}"

if [ "$(id -u)" -ne 0 ]; then
    echo "[错误] 安装 GPIO 状态恢复服务需要 root 权限。" >&2
    exit 1
fi

# 升级不能擅自改变网页中已经选择的开关状态。首次安装才默认启用；后续安装完整保留
# unit 的“是否自启”和“当前是否开启”两种状态。
unit_name="rk3588-gpio-restore.service"
control_unit_name="rk3588-gpio-control.service"
was_installed=0
was_enabled=0
was_active=0
if systemctl cat "${unit_name}" >/dev/null 2>&1; then
    was_installed=1
    systemctl is-enabled --quiet "${unit_name}" && was_enabled=1 || true
    systemctl is-active --quiet "${unit_name}" && was_active=1 || true
fi

cmake -S "${gpio_test_dir}" -B "${gpio_test_dir}/build" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "${gpio_test_dir}/build" --parallel
cmake -S "${service_dir}" -B "${daemon_build_dir}" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "${daemon_build_dir}" --parallel
install -d -m 0755 /var/lib/rk3588-gpio
install -m 0755 "${gpio_test_dir}/build/gpio_test" /usr/local/bin/gpio_test
install -m 0755 "${daemon_build_dir}/rk3588_gpio_control_daemon" \
    /usr/local/sbin/rk3588_gpio_control_daemon
install -m 0644 "${service_dir}/rk3588-gpio-control.service" \
    "/etc/systemd/system/${control_unit_name}"
install -m 0644 "${service_dir}/rk3588-gpio-restore.service" \
    "/etc/systemd/system/${unit_name}"
systemctl daemon-reload
systemctl enable "${control_unit_name}"
systemctl restart "${control_unit_name}"

if [ "${was_installed}" -eq 0 ]; then
    systemctl enable --now "${unit_name}"
    echo "首次安装：正在将继电器 ${relay_safe_pin} 设置并保存为低电平。"
    /usr/local/bin/gpio_test --pin "${relay_safe_pin}" set 0
elif [ "${was_enabled}" -eq 1 ]; then
    systemctl enable "${unit_name}"
    systemctl restart "${unit_name}"
elif [ "${was_active}" -eq 1 ]; then
    systemctl disable "${unit_name}"
    systemctl restart "${unit_name}"
else
    systemctl disable --now "${unit_name}"
fi

if [ "${was_installed}" -eq 0 ]; then
    echo "GPIO 状态恢复服务已安装；${relay_safe_pin} 的安全初始电平为 0。"
else
    echo "GPIO 状态恢复服务已升级；原有开关和 GPIO 电平状态已保留。"
fi
systemctl --no-pager --full status "${unit_name}" || true
