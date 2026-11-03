#!/usr/bin/env bash
set -euo pipefail

tool_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${tool_dir}/../../.." && pwd)"
sdk_dir="${repo_root}/engine/vendor/rknn/2.4.2a2"

# 校验并直接链接仓库锁定的 Runtime，避免回退到系统 RKNN。
(cd "${sdk_dir}" && sha256sum --check SHA256SUMS)
"${CXX:-g++}" -O3 -std=c++14 -pthread \
    -I"${sdk_dir}/include" -I"${repo_root}/engine/src" \
    "${tool_dir}/rknn_profile.cpp" \
    "${sdk_dir}/lib/aarch64/librknnrt.so" \
    -Wl,-rpath,"${sdk_dir}/lib/aarch64" \
    -o "${tool_dir}/rknn_profile"

printf '构建完成：%s/rknn_profile\n' "${tool_dir}"
