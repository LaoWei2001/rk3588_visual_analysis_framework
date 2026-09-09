#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" --parallel "$(nproc)"

echo
echo "构建完成: ${BUILD_DIR}/yolo26_pose_verify"
echo "检查模型: ${BUILD_DIR}/yolo26_pose_verify --model /path/model.rknn --inspect"

