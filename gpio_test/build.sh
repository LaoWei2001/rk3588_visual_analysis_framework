#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cmake -S "${project_dir}" -B "${project_dir}/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "${project_dir}/build" --parallel

echo "构建完成：${project_dir}/build/gpio_test"
echo "设置低电平：sudo ${project_dir}/build/gpio_test set 0"
echo "GPIO 服务会随 Web 控制台自动安装；无 Web 设备可执行："
echo "  sudo ${project_dir}/../service/gpio_state/install.sh"
