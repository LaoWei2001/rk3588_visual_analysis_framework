#!/usr/bin/env bash
# Shared CMake workflow used by each independent project's build.sh.
set -euo pipefail
project_dir="$(cd "${1:?project directory required}" && pwd)"
shift
engine_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
mode=build
if [[ "${1:-}" == build || "${1:-}" == package || "${1:-}" == clean ]]; then
    mode="$1"; shift
fi
build_type=Release
jobs=4
toolchain=""
image=""
clean=false
package_args=()
while (($#)); do
    case "$1" in
        --engine) engine_dir="$(cd "${2:?--engine requires a path}" && pwd)"; shift 2 ;;
        --build-type) build_type="${2:?--build-type requires a value}"; shift 2 ;;
        --jobs|-j) jobs="${2:?--jobs requires a value}"; shift 2 ;;
        --toolchain) toolchain="$(realpath "${2:?--toolchain requires a file}")"; shift 2 ;;
        --image) image="${2:?--image requires a name}"; shift 2 ;;
        --clean) clean=true; shift ;;
        --no-strip|--no-bundle-libs) package_args+=("$1"); shift ;;
        --output) package_args+=("$1" "${2:?--output requires a directory}"); shift 2 ;;
        --help|-h)
            echo 'Usage: ./build.sh [build|package|clean] [--engine DIR] [--build-type Release|Debug|RelWithDebInfo] [--jobs N] [--clean] [--toolchain FILE | --image IMAGE]'
            echo 'Package options: --output DIR --no-strip --no-bundle-libs'
            exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
[[ "$build_type" =~ ^(Release|Debug|RelWithDebInfo)$ ]] || { echo 'Invalid build type' >&2; exit 2; }
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo 'Jobs must be positive' >&2; exit 2; }
[[ -z "$image" || -z "$toolchain" ]] || { echo 'Choose --image or --toolchain' >&2; exit 2; }
if [[ "$mode" != package && ${#package_args[@]} -gt 0 ]]; then
    echo 'Package options require ./build.sh package' >&2; exit 2
fi
revision="$(git -C "$engine_dir" rev-parse HEAD 2>/dev/null || echo source-archive)"
identity="$(printf 'shared-abi1\n%s\n%s\n%s\n%s' "$engine_dir" "$revision" "$toolchain" "$image" | sha256sum)"
version="$(cat "$engine_dir/VERSION")"
build_dir="$project_dir/build/$version-${identity:0:12}-$build_type"
if [[ "$clean" == true || "$mode" == clean ]]; then
    rm -rf -- "$build_dir"
fi
[[ "$mode" != clean ]] || exit 0
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
printf 'Built %s\n' "$build_dir/vision_analysis"
if [[ "$mode" == package ]]; then
    compiler_args=()
    [[ -z "$toolchain" ]] || compiler_args+=(--toolchain "$toolchain")
    [[ -z "$image" ]] || compiler_args+=(--image "$image")
    python3 "$engine_dir/tools/project/vision.py" package "$project_dir" \
        --build-dir "$build_dir" --build-type "$build_type" "${compiler_args[@]}" "${package_args[@]}"
fi
