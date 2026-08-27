#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cmake -S "${project_dir}" -B "${project_dir}/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "${project_dir}/build" --parallel

echo "构建完成：${project_dir}/build/gpio_test"
