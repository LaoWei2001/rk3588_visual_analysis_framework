#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cmake -S "${project_dir}" -B "${project_dir}/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "${project_dir}/build" --parallel

echo "构建完成：${project_dir}/build/relay_test"
echo "读取继电器：${project_dir}/build/relay_test get"
echo "吸合继电器：${project_dir}/build/relay_test output 1"
echo "释放继电器：${project_dir}/build/relay_test output 0"
