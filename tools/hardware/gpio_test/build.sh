#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cmake -S "${project_dir}" -B "${project_dir}/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "${project_dir}/build" --parallel

echo "构建完成：${project_dir}/build/rk3588-gpioctl"
echo "输出低电平：sudo ${project_dir}/build/rk3588-gpioctl output 0"
echo "完整部署请从项目根目录执行："
echo "  sudo ${project_dir}/../../../rkvision platform install offline"
