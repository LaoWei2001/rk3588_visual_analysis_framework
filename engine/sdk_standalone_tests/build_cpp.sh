#!/usr/bin/env bash
# Build all C/C++ sources in one standalone SDK test directory.
set -euo pipefail
if [[ $# -eq 1 && ( "$1" == "-h" || "$1" == "--help" ) ]]; then
    echo "Usage: $0 test_directory"
    echo "Output: test_directory/<directory-name>"
    exit 0
fi
if [[ $# -ne 1 ]]; then
    echo "Usage: $0 test_directory" >&2
    exit 2
fi
sdk_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
if [[ ! -d "$1" ]]; then
    echo "请选择包含 C/C++ 源文件的文件夹: $1" >&2
    exit 2
fi
sdk_source="$(realpath -- "$1")"
if [[ "$sdk_source" == "$sdk_root/engine/sdk_standalone_tests" ]]; then
    echo "请选择具体测试文件夹，例如: $0 model_inspection" >&2
    exit 2
fi
sdk_build="$(realpath -m -- "${VISION_SDK_BUILD_DIR:-$sdk_source/build}")"
if [[ "$sdk_source" == "$sdk_build" ]]; then
    echo "构建目录不能与源文件夹相同" >&2
    exit 2
fi
sdk_name="$(basename -- "$sdk_source")"
cmake -S "$sdk_root/engine/sdk_standalone_tests" -B "$sdk_build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DVISION_SDK_SOURCE_DIR="$sdk_source"
cmake --build "$sdk_build" --target sdk_example -j "${VISION_BUILD_JOBS:-4}"
echo "Built: $sdk_source/$sdk_name"
