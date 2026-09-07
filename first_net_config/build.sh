#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${project_dir}/build"
build_type="Release"

if [[ "${1:-}" == "clean" ]]; then
    cmake -E remove_directory "${build_dir}"
    cmake -E remove -f "${project_dir}/first_net_config"
fi

cmake \
    -S "${project_dir}/src" \
    -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE="${build_type}"

cmake --build "${build_dir}" --parallel

echo
echo "构建完成：${project_dir}/first_net_config"
