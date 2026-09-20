#!/usr/bin/env bash
# Internal shared CMake builder invoked only by the RKVision command.
set -euo pipefail
project_dir="$(cd "${1:?project directory required}" && pwd)"
shift
engine_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_type=Release
jobs=4
toolchain=""
image=""
clean=false
while (($#)); do
    case "$1" in
        --engine) engine_dir="$(cd "${2:?--engine requires a path}" && pwd)"; shift 2 ;;
        --build-type) build_type="${2:?--build-type requires a value}"; shift 2 ;;
        --jobs|-j) jobs="${2:?--jobs requires a value}"; shift 2 ;;
        --toolchain) toolchain="$(realpath "${2:?--toolchain requires a file}")"; shift 2 ;;
        --image) image="${2:?--image requires a name}"; shift 2 ;;
        --clean) clean=true; shift ;;
        --help|-h)
            echo '用法：build.sh PROJECT_DIR [--engine DIR] [--build-type Release|Debug|RelWithDebInfo] [--jobs N] [--clean] [--toolchain FILE | --image IMAGE]'
            exit 0 ;;
        *) echo "未知参数：$1" >&2; exit 2 ;;
    esac
done
[[ "$build_type" =~ ^(Release|Debug|RelWithDebInfo)$ ]] || { echo '构建类型无效' >&2; exit 2; }
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo '并行任务数必须是正整数' >&2; exit 2; }
[[ -z "$image" || -z "$toolchain" ]] || { echo '--image 和 --toolchain 只能选择一个' >&2; exit 2; }
revision="$(git -C "$engine_dir" rev-parse HEAD 2>/dev/null || echo source-archive)"
identity="$(printf 'shared-abi1\n%s\n%s\n%s\n%s' "$engine_dir" "$revision" "$toolchain" "$image" | sha256sum)"
version="$(cat "$engine_dir/VERSION")"
build_dir="$project_dir/build/$version-${identity:0:12}-$build_type"
if [[ "$clean" == true ]]; then
    rm -rf -- "$build_dir"
fi
mkdir -p "$build_dir"
# Serialize the shared cache build and staging; concurrent project builds cannot
# copy a half-written runtime. Project clean never removes the shared cache.
runtime_dir="$engine_dir/build/runtime/$version-${identity:0:12}-$build_type"
mkdir -p "$runtime_dir"
exec 9>"$runtime_dir/build.lock"
flock 9
arch="$(uname -m)"
if [[ -n "$image" || ( "$arch" != aarch64 && "$arch" != armv7l && -z "$toolchain" ) ]]; then
    docker_image="${image:-rk3588_builder:2026_4_30}"
    container_build="/project/build/$(basename "$build_dir")"
    cp "$engine_dir/tools/project/aarch64-docker.cmake" "$runtime_dir/cross.cmake"
    docker_mounts=(-v "$engine_dir:/engine:ro" -v "$project_dir:/project" -v "$runtime_dir:/runtime")
    docker run --rm "${docker_mounts[@]}" "$docker_image" \
        cmake -S /engine/vision_analysis -B /runtime -DCMAKE_BUILD_TYPE="$build_type" \
        -DCMAKE_TOOLCHAIN_FILE=/runtime/cross.cmake
    docker run --rm "${docker_mounts[@]}" "$docker_image" cmake --build /runtime --parallel "$jobs"
    docker run --rm "${docker_mounts[@]}" "$docker_image" \
        cmake -S /project -B "$container_build" -DRKVISION_ENGINE=/engine \
        -DRKVISION_RUNTIME_DIR=/runtime/libs -DCMAKE_BUILD_TYPE="$build_type" \
        -DCMAKE_TOOLCHAIN_FILE=/runtime/cross.cmake
    docker run --rm "${docker_mounts[@]}" "$docker_image" \
        cmake --build "$container_build" --parallel "$jobs"
else
    compiler_args=()
    [[ -z "$toolchain" ]] || compiler_args+=("-DCMAKE_TOOLCHAIN_FILE=$toolchain")
    cmake -S "$engine_dir/vision_analysis" -B "$runtime_dir" \
        "-DCMAKE_BUILD_TYPE=$build_type" "${compiler_args[@]}"
    cmake --build "$runtime_dir" --parallel "$jobs"
    cmake -S "$project_dir" -B "$build_dir" "-DRKVISION_ENGINE=$engine_dir" \
        "-DRKVISION_RUNTIME_DIR=$runtime_dir/libs" "-DCMAKE_BUILD_TYPE=$build_type" "${compiler_args[@]}"
    cmake --build "$build_dir" --parallel "$jobs"
fi
flock -u 9
printf '已构建 %s\n' "$build_dir/vision_analysis"
