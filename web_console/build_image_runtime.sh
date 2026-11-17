#!/usr/bin/env bash
# Build/stage the console-owned engine. No projects/ or installed app is needed.
set -Eeuo pipefail
if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <runtime destination>" >&2
    exit 2
fi
image_project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image_build_dir="${IMAGE_INFERENCE_BUILD_DIR:-$image_project_root/build/image-inference}"
image_destination="$1"
cmake -S "$image_project_root/engine" -B "$image_build_dir" -DVISION_INFERENCE_ONLY=ON
cmake --build "$image_build_dir" --target image_inference_runner -j "${IMAGE_INFERENCE_BUILD_JOBS:-4}"
mkdir -p "$image_destination/libs"
install -m 0755 "$image_build_dir/image_inference_runner" "$image_destination/image_inference_runner.new"
install -m 0644 "$image_project_root/engine/vendor/rknn/2.4.2a2/lib/aarch64/librknnrt.so" "$image_destination/libs/librknnrt.so.new"
mv -f "$image_destination/libs/librknnrt.so.new" "$image_destination/libs/librknnrt.so"
mv -f "$image_destination/image_inference_runner.new" "$image_destination/image_inference_runner"
"$image_destination/image_inference_runner" --image-inference-capabilities
