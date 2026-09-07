#!/usr/bin/env bash
# 在有网 ARM64 开发机上生成可直接由 APT 安装的项目离线依赖仓库。
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DETECTOR="$SCRIPT_DIR/detect_apt_dependencies.sh"
FRONTEND_DIR="$PROJECT_ROOT/web_console/frontend"
OUTPUT_DIR="$SCRIPT_DIR/output"
FINAL_BUNDLE_DIR="$OUTPUT_DIR/bundle"

WANT_BUILD=true
REFRESH_DEBS=false
ADDED_RUNTIME=()
ADDED_BUILD=()
STAGE_DIR=""
BUILD_SUCCEEDED=false

usage() {
    cat <<'EOF'
用法：bash offline_install_env_debian/create_bundle.sh [选项]

选项：
  --build                 兼容参数；默认已经包含 C/C++ 编译环境
  --runtime-only          只打包运行环境，不包含板端编译工具
  --add <APT包名>         添加一个无法自动识别的运行依赖并永久记入清单
  --add-build <APT包名>   添加一个编译依赖并永久记入清单
  --refresh-debs          不复用上一版 deb，按当前软件源全量刷新
  -h, --help              显示帮助

脚本会自动收集基础清单、extra-*-packages.txt、Python requirements，
并从项目中已经构建的 ELF 文件识别其动态库所属 Debian 包。
EOF
}

valid_package_name() {
    [[ "$1" =~ ^[a-z0-9][a-z0-9+.-]+$ ]]
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build) WANT_BUILD=true ;;
        --runtime-only) WANT_BUILD=false ;;
        --refresh-debs) REFRESH_DEBS=true ;;
        --add|--add-build)
            option="$1"
            [ "$#" -ge 2 ] || { echo "[错误] $option 缺少 APT 包名" >&2; exit 2; }
            valid_package_name "$2" || { echo "[错误] APT 包名无效: $2" >&2; exit 2; }
            if [ "$option" = "--add" ]; then
                ADDED_RUNTIME+=("$2")
            else
                ADDED_BUILD+=("$2")
            fi
            shift
            ;;
        -h|--help) usage; exit 0 ;;
        *) echo "[错误] 未知参数: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

if [ "${#ADDED_BUILD[@]}" -gt 0 ] && [ "$WANT_BUILD" != true ]; then
    echo "[错误] --add-build 不能和 --runtime-only 一起使用。" >&2
    exit 2
fi

# shellcheck source=dependency_manifest.sh
source "$SCRIPT_DIR/dependency_manifest.sh"

required_commands=(
    apt-cache apt-get awk dpkg dpkg-deb dpkg-query dpkg-scanpackages find grep gzip
    ldconfig mktemp npm python3 readelf sed sort tar xargs dpkg-repack
)
for command_name in "${required_commands[@]}"; do
    command -v "$command_name" >/dev/null 2>&1 \
        || { echo "[错误] 制作机缺少命令: $command_name" >&2; exit 1; }
done
python3 -m pip --version >/dev/null 2>&1 \
    || { echo "[错误] 制作机缺少 python3-pip" >&2; exit 1; }
[ -x "$DETECTOR" ] || chmod +x "$DETECTOR"

MACHINE_ARCH="$(uname -m)"
DEB_ARCH="$(dpkg --print-architecture)"
if [ "$MACHINE_ARCH" != "aarch64" ] || [ "$DEB_ARCH" != "arm64" ]; then
    echo "[错误] 请在 ARM64 开发机上制包；当前 uname=$MACHINE_ARCH, dpkg=$DEB_ARCH" >&2
    exit 1
fi

# shellcheck source=/etc/os-release
source /etc/os-release
OS_ID="${ID:-unknown}"
OS_VERSION_ID="${VERSION_ID:-unknown}"
PYTHON_ABI="$(python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
DEB_VERSION="1.0.${STAMP//[TZ]/}"

if [ "$(id -u)" -eq 0 ]; then
    ROOT=()
elif command -v sudo >/dev/null 2>&1; then
    ROOT=(sudo)
else
    echo "[错误] 刷新 APT 索引需要 root 或 sudo。" >&2
    exit 1
fi
as_root() { "${ROOT[@]}" "$@"; }

cleanup() {
    if [ -n "$STAGE_DIR" ] && [ -d "$STAGE_DIR" ]; then
        rm -rf -- "$STAGE_DIR"
    fi
    if [ "$BUILD_SUCCEEDED" != true ]; then
        echo "[提示] 制包未完成，原 output/bundle 未改变。" >&2
    fi
}
trap cleanup EXIT

mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"
REPACK_CACHE_DIR="$OUTPUT_DIR/repack-cache"
mkdir -p "$REPACK_CACHE_DIR"
STAGE_DIR="$(mktemp -d "$OUTPUT_DIR/.offline-build.XXXXXX")"
BUNDLE_DIR="$STAGE_DIR/bundle"
APT_DIR="$BUNDLE_DIR/apt"
WORK_DIR="$STAGE_DIR/work"
mkdir -p "$APT_DIR" "$WORK_DIR"

echo ">>> [1/7] 更新索引并识别项目依赖..."
SOURCE_APT_OPTIONS=()
if [ "$OS_ID" = debian ] && [ -n "${VERSION_CODENAME:-}" ]; then
    # 使用独立的 Debian 官方索引，不修改开发机 /etc/apt，也不受已经失效的
    # 第三方镜像或 backports 条目影响。解析与下载始终使用同一份索引。
    SOURCE_APT_ROOT="$WORK_DIR/source-apt"
    mkdir -p "$SOURCE_APT_ROOT/lists/partial" "$SOURCE_APT_ROOT/sourceparts" \
        "$SOURCE_APT_ROOT/cache/archives/partial"
    cat > "$SOURCE_APT_ROOT/sources.list" <<EOF
deb https://deb.debian.org/debian ${VERSION_CODENAME} main contrib non-free
deb https://deb.debian.org/debian ${VERSION_CODENAME}-updates main contrib non-free
deb https://deb.debian.org/debian-security ${VERSION_CODENAME}-security main contrib non-free
deb [check-valid-until=no] https://snapshot.debian.org/archive/debian-security/${DEBIAN_SECURITY_SNAPSHOT}/ ${VERSION_CODENAME}-security main contrib non-free
EOF
    SOURCE_APT_OPTIONS=(
        -o Debug::NoLocking=true
        -o "APT::Sandbox::User=$(id -un)"
        -o "Dir::Etc::sourcelist=$SOURCE_APT_ROOT/sources.list"
        -o "Dir::Etc::sourceparts=$SOURCE_APT_ROOT/sourceparts"
        -o "Dir::State::lists=$SOURCE_APT_ROOT/lists"
        -o "Dir::Cache::archives=$SOURCE_APT_ROOT/cache/archives"
        -o Acquire::Languages=none
        -o Acquire::Check-Valid-Until=false
        -o Acquire::IndexTargets::deb::Contents-deb::DefaultEnabled=false
        -o Acquire::https::Timeout=30
    )
    apt-get "${SOURCE_APT_OPTIONS[@]}" update \
        || { echo "[错误] 无法更新隔离的 Debian 官方软件源索引。" >&2; exit 1; }
else
    as_root apt-get update \
        || { echo "[错误] APT 软件源索引更新失败。" >&2; exit 1; }
fi

declare -A DOWNLOADABLE_APT_PACKAGES=()
while IFS= read -r package; do
    [ -n "$package" ] && DOWNLOADABLE_APT_PACKAGES["$package"]=1
done < <(apt-cache "${SOURCE_APT_OPTIONS[@]}" dumpavail 2>/dev/null \
    | sed -n 's/^Package: //p' | LC_ALL=C sort -u)

# 旧版脚本曾把 Debian 官方包重封装进缓存，既占空间，也可能把开发机上的
# 旧版本混入新仓库。缓存现在只允许保存软件源中不存在的本地厂商包。
pruned_cache_count=0
while IFS= read -r -d '' cached_deb; do
    cached_package="$(dpkg-deb -f "$cached_deb" Package 2>/dev/null || true)"
    if [ -z "$cached_package" ] \
            || [ -n "${DOWNLOADABLE_APT_PACKAGES[$cached_package]+set}" ]; then
        rm -f -- "$cached_deb"
        pruned_cache_count=$((pruned_cache_count + 1))
    fi
done < <(find "$REPACK_CACHE_DIR" -maxdepth 1 -type f -name '*.deb' -print0)
[ "$pruned_cache_count" -eq 0 ] \
    || echo "    已清理 $pruned_cache_count 个旧版官方包缓存。"

RUNTIME_DIRECT="$WORK_DIR/runtime-direct-packages.txt"
ALL_DIRECT="$WORK_DIR/all-direct-packages.txt"
bash "$DETECTOR" > "$RUNTIME_DIRECT"
if [ "${#ADDED_RUNTIME[@]}" -gt 0 ]; then
    printf '%s\n' "${ADDED_RUNTIME[@]}" >> "$RUNTIME_DIRECT"
fi
LC_ALL=C sort -u -o "$RUNTIME_DIRECT" "$RUNTIME_DIRECT"

if [ "$WANT_BUILD" = true ]; then
    bash "$DETECTOR" --build > "$ALL_DIRECT"
    if [ "${#ADDED_RUNTIME[@]}" -gt 0 ]; then
        printf '%s\n' "${ADDED_RUNTIME[@]}" >> "$ALL_DIRECT"
    fi
    if [ "${#ADDED_BUILD[@]}" -gt 0 ]; then
        printf '%s\n' "${ADDED_BUILD[@]}" >> "$ALL_DIRECT"
    fi
    LC_ALL=C sort -u -o "$ALL_DIRECT" "$ALL_DIRECT"
else
    cp "$RUNTIME_DIRECT" "$ALL_DIRECT"
fi
LC_ALL=C comm -13 "$RUNTIME_DIRECT" "$ALL_DIRECT" > "$WORK_DIR/build-only-packages.txt"
cp "$RUNTIME_DIRECT" "$APT_DIR/runtime-direct-packages.txt"
cp "$WORK_DIR/build-only-packages.txt" "$APT_DIR/build-direct-packages.txt"

if [ -f "$FINAL_BUNDLE_DIR/apt/runtime-direct-packages.txt" ]; then
    NEW_PACKAGES="$(LC_ALL=C comm -13 \
        "$FINAL_BUNDLE_DIR/apt/runtime-direct-packages.txt" "$RUNTIME_DIRECT" || true)"
    if [ -n "$NEW_PACKAGES" ]; then
        echo "    本次新识别的运行依赖："
        printf '%s\n' "$NEW_PACKAGES" | sed 's/^/      + /'
    fi
fi
echo "    直接运行依赖 $(wc -l < "$RUNTIME_DIRECT") 个。"

echo ">>> [2/7] 收集本地厂商包并下载 Debian 完整依赖闭包..."
OFFICIAL_DIRECT="$WORK_DIR/official-direct-packages.txt"
LOCAL_SEEDS="$WORK_DIR/local-seed-packages.txt"
: > "$OFFICIAL_DIRECT"
: > "$LOCAL_SEEDS"
while IFS= read -r package; do
    [ -n "$package" ] || continue
    if [ -n "${DOWNLOADABLE_APT_PACKAGES[$package]+set}" ]; then
        printf '%s\n' "$package" >> "$OFFICIAL_DIRECT"
    elif dpkg-query -W -f='${db:Status-Status}' "$package" 2>/dev/null \
            | grep -qx installed; then
        printf '%s\n' "$package" >> "$LOCAL_SEEDS"
    else
        echo "[错误] 依赖 $package 既不在 APT 软件源中，也没有安装在开发机上。" >&2
        exit 1
    fi
done < "$ALL_DIRECT"
LC_ALL=C sort -u -o "$OFFICIAL_DIRECT" "$OFFICIAL_DIRECT"
LC_ALL=C sort -u -o "$LOCAL_SEEDS" "$LOCAL_SEEDS"

# 只有 APT 软件源中不存在的本地厂商包才使用 dpkg-repack。Debian 官方包
# 始终按空系统解析出的候选版本下载，避免开发机旧版本与软件源新版本混用。
declare -A LOCAL_REPACKED_VERSIONS=()
declare -A LOCAL_REPACKED_DEBS=()
repack_local_package() {
    local package="$1"
    local installed_version cached_deb candidate_deb repack_dir repack_log
    [ -z "${LOCAL_REPACKED_VERSIONS[$package]+set}" ] || return 0
    dpkg-query -W -f='${db:Status-Status}' "$package" 2>/dev/null \
        | grep -qx installed || return 1
    installed_version="$(dpkg-query -W -f='${Version}' "$package")"

    if [ "$REFRESH_DEBS" != true ]; then
        while IFS= read -r cached_deb; do
            if [ "$(dpkg-deb -f "$cached_deb" Package 2>/dev/null || true)" = "$package" ] \
                    && [ "$(dpkg-deb -f "$cached_deb" Version 2>/dev/null || true)" = "$installed_version" ]; then
                cp "$cached_deb" "$APT_DIR/"
                candidate_deb="$APT_DIR/$(basename "$cached_deb")"
                LOCAL_REPACKED_VERSIONS["$package"]="$installed_version"
                LOCAL_REPACKED_DEBS["$package"]="$candidate_deb"
                return 0
            fi
        done < <(find "$REPACK_CACHE_DIR" -maxdepth 1 -type f -name '*.deb')
    fi

    repack_dir="$WORK_DIR/repack/$package"
    repack_log="$WORK_DIR/repack-$package.log"
    mkdir -p "$repack_dir"
    (cd "$repack_dir" && dpkg-repack "$package") >"$repack_log" 2>&1 || true
    candidate_deb=""
    while IFS= read -r generated_deb; do
        if [ "$(dpkg-deb -f "$generated_deb" Package 2>/dev/null || true)" = "$package" ] \
                && [ "$(dpkg-deb -f "$generated_deb" Version 2>/dev/null || true)" = "$installed_version" ]; then
            candidate_deb="$generated_deb"
            break
        fi
    done < <(find "$repack_dir" -maxdepth 1 -type f -name '*.deb')
    if [ -z "$candidate_deb" ]; then
        echo "[错误] 无法从开发机重新封装本地包: $package=$installed_version" >&2
        sed -n '1,30p' "$repack_log" | sed 's/^/       /' >&2
        return 1
    fi
    cp "$candidate_deb" "$REPACK_CACHE_DIR/"
    cp "$candidate_deb" "$APT_DIR/"
    candidate_deb="$APT_DIR/$(basename "$candidate_deb")"
    LOCAL_REPACKED_VERSIONS["$package"]="$installed_version"
    LOCAL_REPACKED_DEBS["$package"]="$candidate_deb"
}

mapfile -t LOCAL_QUEUE < "$LOCAL_SEEDS"
for ((local_index=0; local_index<${#LOCAL_QUEUE[@]}; ++local_index)); do
    package="${LOCAL_QUEUE[$local_index]}"
    [ -z "${LOCAL_REPACKED_VERSIONS[$package]+set}" ] || continue
    repack_local_package "$package" || exit 1

    # 把本地包依赖的 Debian 包加入官方解析；若依赖仍只存在于开发机，
    # 则递归作为本地包封装。最终仍由本地 APT 模拟检查完整关系。
    while IFS= read -r dependency; do
        dependency="${dependency%%:*}"
        [ -n "$dependency" ] || continue
        if [ -n "${DOWNLOADABLE_APT_PACKAGES[$dependency]+set}" ]; then
            printf '%s\n' "$dependency" >> "$OFFICIAL_DIRECT"
        elif dpkg-query -W -f='${db:Status-Status}' "$dependency" 2>/dev/null \
                | grep -qx installed; then
            LOCAL_QUEUE+=("$dependency")
        fi
    done < <(
        dpkg-deb -f "${LOCAL_REPACKED_DEBS[$package]}" Pre-Depends Depends 2>/dev/null \
            | grep -oE '(^|[|,])[[:space:]]*[a-z0-9][a-z0-9+.-]*(:[a-z0-9]+)?' \
            | sed -E 's/^[|,]?[[:space:]]*//' \
            | LC_ALL=C sort -u \
            || true
    )
done
LC_ALL=C sort -u -o "$OFFICIAL_DIRECT" "$OFFICIAL_DIRECT"

# 本轮所需本地包都已确定，只保留与开发机当前版本完全一致的厂商包缓存。
declare -A KEPT_LOCAL_CACHE_SPECS=()
while IFS= read -r -d '' cached_deb; do
    cached_package="$(dpkg-deb -f "$cached_deb" Package 2>/dev/null || true)"
    cached_version="$(dpkg-deb -f "$cached_deb" Version 2>/dev/null || true)"
    cached_key="$cached_package=$cached_version"
    if [ -z "${LOCAL_REPACKED_VERSIONS[$cached_package]+set}" ] \
            || [ "$cached_version" != "${LOCAL_REPACKED_VERSIONS[$cached_package]:-}" ] \
            || [ -n "${KEPT_LOCAL_CACHE_SPECS[$cached_key]+set}" ]; then
        rm -f -- "$cached_deb"
    else
        KEPT_LOCAL_CACHE_SPECS["$cached_key"]=1
    fi
done < <(find "$REPACK_CACHE_DIR" -maxdepth 1 -type f -name '*.deb' -print0)

EMPTY_STATUS="$WORK_DIR/empty-dpkg-status"
: > "$EMPTY_STATUS"
mapfile -t OFFICIAL_DIRECT_PACKAGES < "$OFFICIAL_DIRECT"
APT_PLAN="$WORK_DIR/apt-plan.txt"
if ! LC_ALL=C apt-get --simulate \
        "${SOURCE_APT_OPTIONS[@]}" \
        -o Debug::NoLocking=true \
        -o "Dir::State::status=$EMPTY_STATUS" \
        --no-install-recommends install "${OFFICIAL_DIRECT_PACKAGES[@]}" \
        >"$APT_PLAN" 2>&1; then
    cat "$APT_PLAN" >&2
    echo "[错误] APT 无法为近乎空白的 Debian 解析官方依赖。" >&2
    exit 1
fi
awk '$1 == "Inst" {
        package=$2; sub(/:[^:]+$/, "", package)
        version=$3; sub(/^\(/, "", version)
        print package "=" version
    }' \
    "$APT_PLAN" | LC_ALL=C sort -u > "$APT_DIR/official-resolved-packages.txt"
mapfile -t RESOLVED_PACKAGES < "$APT_DIR/official-resolved-packages.txt"
[ "${#RESOLVED_PACKAGES[@]}" -gt 0 ] \
    || { echo "[错误] APT 没有解析出依赖。" >&2; exit 1; }

LOCAL_RESOLVED="$APT_DIR/local-repacked-packages.txt"
: > "$LOCAL_RESOLVED"
for package in "${!LOCAL_REPACKED_VERSIONS[@]}"; do
    printf '%s=%s\n' "$package" "${LOCAL_REPACKED_VERSIONS[$package]}" >> "$LOCAL_RESOLVED"
done
LC_ALL=C sort -u -o "$LOCAL_RESOLVED" "$LOCAL_RESOLVED"
cat "$APT_DIR/official-resolved-packages.txt" "$LOCAL_RESOLVED" \
    | LC_ALL=C sort -u > "$APT_DIR/resolved-packages.txt"

declare -A REQUIRED_VERSIONS=()
while IFS='=' read -r package version; do
    [ -n "$package" ] && REQUIRED_VERSIONS["${package%%:*}"]="$version"
done < "$APT_DIR/resolved-packages.txt"

# 增量复用只接受本轮仍需要、且版本完全相同的 deb；不会再把废弃包或旧版本
# 无条件带入新仓库。
if [ "$REFRESH_DEBS" != true ] && [ -d "$FINAL_BUNDLE_DIR/apt" ]; then
    reused=0
    declare -A REUSED_PACKAGE_KEYS=()
    while IFS= read -r -d '' old_deb; do
        package="$(dpkg-deb -f "$old_deb" Package 2>/dev/null || true)"
        version="$(dpkg-deb -f "$old_deb" Version 2>/dev/null || true)"
        [ -n "${REQUIRED_VERSIONS[$package]+set}" ] || continue
        [ "$version" = "${REQUIRED_VERSIONS[$package]}" ] || continue
        key="$package=$version"
        [ -z "${REUSED_PACKAGE_KEYS[$key]+set}" ] || continue
        old_name="$(basename "$old_deb")"
        if ln "$old_deb" "$APT_DIR/$old_name" 2>/dev/null \
                || cp "$old_deb" "$APT_DIR/$old_name"; then
            REUSED_PACKAGE_KEYS["$key"]=1
            reused=$((reused + 1))
        fi
    done < <(find "$FINAL_BUNDLE_DIR/apt" -maxdepth 1 -type f -name '*.deb' -print0)
    echo "    已从上一版仓库复用 $reused 个 deb。"
fi

declare -A AVAILABLE_DEB_SPECS=()
while IFS= read -r -d '' candidate_deb; do
    package="$(dpkg-deb -f "$candidate_deb" Package 2>/dev/null || true)"
    version="$(dpkg-deb -f "$candidate_deb" Version 2>/dev/null || true)"
    [ -n "$package" ] && [ -n "$version" ] \
        && AVAILABLE_DEB_SPECS["$package=$version"]=1
done < <(find "$APT_DIR" -maxdepth 1 -type f -name '*.deb' -print0)
MISSING_PACKAGES=()
for spec in "${RESOLVED_PACKAGES[@]}"; do
    [ -n "${AVAILABLE_DEB_SPECS[$spec]+set}" ] || MISSING_PACKAGES+=("$spec")
done

for ((start=0; start<${#MISSING_PACKAGES[@]}; start+=20)); do
    batch=("${MISSING_PACKAGES[@]:start:20}")
    batch_ok=false
    for attempt in 1 2 3; do
        if (
            cd "$APT_DIR"
            apt-get "${SOURCE_APT_OPTIONS[@]}" download -qq \
                -o Acquire::Retries=3 "${batch[@]}"
        ); then
            batch_ok=true
            break
        fi
        echo "    [警告] 下载失败，重试 $attempt/3..." >&2
    done
    [ "$batch_ok" = true ] \
        || { echo "[错误] 软件源连续失败；上一版仓库仍保持不变。" >&2; exit 1; }
    echo "    新下载 $((start + ${#batch[@]}))/${#MISSING_PACKAGES[@]}"
done

# 下载工具返回成功并不等于仓库内容一定完整，再按包名和精确版本逐项验收。
declare -A VERIFIED_SPECS=()
while IFS= read -r -d '' candidate_deb; do
    package="$(dpkg-deb -f "$candidate_deb" Package 2>/dev/null || true)"
    version="$(dpkg-deb -f "$candidate_deb" Version 2>/dev/null || true)"
    key="$package=$version"
    [ -n "${REQUIRED_VERSIONS[$package]+set}" ] \
        && [ "$version" = "${REQUIRED_VERSIONS[$package]}" ] || continue
    if [ -n "${VERIFIED_SPECS[$key]+set}" ]; then
        rm -f -- "$candidate_deb"
    else
        VERIFIED_SPECS["$key"]=1
    fi
done < <(find "$APT_DIR" -maxdepth 1 -type f -name '*.deb' -print0)
for spec in "${!REQUIRED_VERSIONS[@]}"; do
    key="$spec=${REQUIRED_VERSIONS[$spec]}"
    [ -n "${VERIFIED_SPECS[$key]+set}" ] \
        || { echo "[错误] 仓库缺少精确版本 deb: $key" >&2; exit 1; }
done
echo "    官方包 ${#RESOLVED_PACKAGES[@]} 个，本地厂商包 ${#LOCAL_REPACKED_VERSIONS[@]} 个。"
echo "    本次需要下载 ${#MISSING_PACKAGES[@]} 个官方 deb。"

echo ">>> [3/7] 将 Python requirements 封装为 deb..."
WHEELHOUSE="$WORK_DIR/wheelhouse"
REUSED_WHEELS="$WORK_DIR/reused-wheels"
REQ_DIR="$WORK_DIR/requirements"
mkdir -p "$WHEELHOUSE" "$REUSED_WHEELS" "$REQ_DIR"
PIP_REQUIREMENT_ARGS=()
for requirement_relative in "${PYTHON_REQUIREMENTS[@]}"; do
    requirement_source="$PROJECT_ROOT/$requirement_relative"
    [ -f "$requirement_source" ] \
        || { echo "[错误] requirements 不存在: $requirement_relative" >&2; exit 1; }
    requirement_name="${requirement_relative//\//__}"
    cp "$requirement_source" "$REQ_DIR/$requirement_name"
    PIP_REQUIREMENT_ARGS+=(-r "$requirement_source")
done

# 兼容旧版目录式 wheelhouse，也兼容新版 python-deps deb 中携带的 wheelhouse。
if [ -d "$FINAL_BUNDLE_DIR/python/wheelhouse" ]; then
    cp -a "$FINAL_BUNDLE_DIR/python/wheelhouse/." "$REUSED_WHEELS/"
elif [ -d "$FINAL_BUNDLE_DIR/apt" ]; then
    previous_python_deb="$(find "$FINAL_BUNDLE_DIR/apt" -maxdepth 1 -type f \
        -name 'vision-analysis-python-deps_*.deb' | LC_ALL=C sort | tail -n 1)"
    if [ -n "$previous_python_deb" ]; then
        PREVIOUS_PYTHON_ROOT="$WORK_DIR/previous-python-deb"
        dpkg-deb -x "$previous_python_deb" "$PREVIOUS_PYTHON_ROOT"
        if [ -d "$PREVIOUS_PYTHON_ROOT/usr/share/vision-analysis/python/wheelhouse" ]; then
            cp -a "$PREVIOUS_PYTHON_ROOT/usr/share/vision-analysis/python/wheelhouse/." \
                "$REUSED_WHEELS/"
        fi
    fi
fi

WHEELS_REUSED=false
if find "$REUSED_WHEELS" -maxdepth 1 -type f -print -quit | grep -q .; then
    if python3 -m pip download --dest "$WHEELHOUSE" \
            --no-index --find-links "$REUSED_WHEELS" \
            --only-binary=:all: --prefer-binary \
            pip setuptools wheel "${PIP_REQUIREMENT_ARGS[@]}"; then
        WHEELS_REUSED=true
        echo "    requirements 未新增缺失项，Python wheels 全部复用。"
    fi
fi
if [ "$WHEELS_REUSED" != true ]; then
    python3 -m pip download --dest "$WHEELHOUSE" \
        --find-links "$REUSED_WHEELS" --only-binary=:all: --prefer-binary \
        pip setuptools wheel "${PIP_REQUIREMENT_ARGS[@]}"
fi

PY_PACKAGE="vision-analysis-python-deps"
PY_ROOT="$WORK_DIR/$PY_PACKAGE"
mkdir -p "$PY_ROOT/DEBIAN" "$PY_ROOT/usr/share/vision-analysis/python"
cp -a "$WHEELHOUSE" "$REQ_DIR" "$PY_ROOT/usr/share/vision-analysis/python/"
cat > "$PY_ROOT/DEBIAN/control" <<EOF
Package: $PY_PACKAGE
Version: $DEB_VERSION
Architecture: $DEB_ARCH
Depends: python3, python3-venv
Maintainer: Vision Analysis Project
Description: Isolated Python environment for vision analysis
EOF
cat > "$PY_ROOT/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
base=/opt/vision-analysis
target=$base/python-env
stage=$base/.python-env.new
share=/usr/share/vision-analysis/python
rm -rf "$stage"
mkdir -p "$base"
python3 -m venv "$stage"
"$stage/bin/pip" install --no-index --find-links "$share/wheelhouse" \
    --upgrade pip setuptools wheel
for requirement in "$share"/requirements/*; do
    [ -f "$requirement" ] || continue
    "$stage/bin/pip" install --no-index --find-links "$share/wheelhouse" \
        --prefer-binary -r "$requirement"
done
"$stage/bin/pip" check
rm -rf "$target"
mv "$stage" "$target"
EOF
cat > "$PY_ROOT/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e
if [ "${1:-}" = remove ]; then
    rm -rf /opt/vision-analysis/python-env
fi
EOF
chmod 0755 "$PY_ROOT/DEBIAN/postinst" "$PY_ROOT/DEBIAN/prerm"
dpkg-deb --build --root-owner-group "$PY_ROOT" "$APT_DIR/${PY_PACKAGE}_${DEB_VERSION}_${DEB_ARCH}.deb" >/dev/null

echo ">>> [4/7] 构建前端并封装为 deb..."
[ -f "$FRONTEND_DIR/package-lock.json" ] \
    || { echo "[错误] 缺少前端 package-lock.json" >&2; exit 1; }
FRONTEND_WORK="$WORK_DIR/frontend-work"
if [ -d "$FRONTEND_DIR/node_modules" ] \
        && (cd "$FRONTEND_DIR" && npm ls --depth=0 >/dev/null 2>&1); then
    echo "    使用开发机已有 node_modules 构建，不访问 npm registry。"
    (cd "$FRONTEND_DIR" && npm run build)
    FRONTEND_WORK="$FRONTEND_DIR"
else
    mkdir -p "$FRONTEND_WORK"
    (
        cd "$FRONTEND_DIR"
        tar --exclude='./node_modules' --exclude='./dist' --exclude='*.tsbuildinfo' -cf - .
    ) | tar -xf - -C "$FRONTEND_WORK"
    (cd "$FRONTEND_WORK" && npm ci --no-audit --no-fund && npm run build)
fi
[ -f "$FRONTEND_WORK/dist/index.html" ] \
    || { echo "[错误] 前端构建没有生成 dist/index.html" >&2; exit 1; }

FRONTEND_PACKAGE="vision-analysis-frontend-assets"
FRONTEND_ROOT="$WORK_DIR/$FRONTEND_PACKAGE"
mkdir -p "$FRONTEND_ROOT/DEBIAN" "$FRONTEND_ROOT/usr/share/vision-analysis/frontend"
cp -a "$FRONTEND_WORK/dist" "$FRONTEND_ROOT/usr/share/vision-analysis/frontend/"
cat > "$FRONTEND_ROOT/DEBIAN/control" <<EOF
Package: $FRONTEND_PACKAGE
Version: $DEB_VERSION
Architecture: all
Maintainer: Vision Analysis Project
Description: Prebuilt web frontend for vision analysis
EOF
dpkg-deb --build --root-owner-group "$FRONTEND_ROOT" \
    "$APT_DIR/${FRONTEND_PACKAGE}_${DEB_VERSION}_all.deb" >/dev/null

# 项目中固定版本、但不属于系统 dpkg 包的 RKNN 用户态文件也封装成 deb。
# 文件放在独立目录，不覆盖厂家镜像可能已有的 /usr/lib 内容。
ROCKCHIP_FILES_PACKAGE="vision-analysis-rockchip-files"
ROCKCHIP_FILES_ROOT="$WORK_DIR/$ROCKCHIP_FILES_PACKAGE"
mkdir -p "$ROCKCHIP_FILES_ROOT/DEBIAN" \
    "$ROCKCHIP_FILES_ROOT/etc/ld.so.conf.d"
for file_mapping in "${BUNDLED_RUNTIME_FILES[@]}"; do
    source_relative="${file_mapping%%|*}"
    install_target="${file_mapping#*|}"
    source_file="$PROJECT_ROOT/$source_relative"
    if [ "$source_relative" = "$file_mapping" ] || [[ "$install_target" != /* ]]; then
        echo "[错误] BUNDLED_RUNTIME_FILES 格式无效: $file_mapping" >&2
        exit 1
    fi
    [ -f "$source_file" ] \
        || { echo "[错误] 待封装文件不存在: $source_relative" >&2; exit 1; }
    mkdir -p "$ROCKCHIP_FILES_ROOT/${install_target%/*}"
    cp -a "$source_file" "$ROCKCHIP_FILES_ROOT/$install_target"
done
printf '%s\n' '/opt/vision-analysis/rockchip/lib' \
    > "$ROCKCHIP_FILES_ROOT/etc/ld.so.conf.d/vision-analysis-rockchip.conf"
cat > "$ROCKCHIP_FILES_ROOT/DEBIAN/control" <<EOF
Package: $ROCKCHIP_FILES_PACKAGE
Version: $DEB_VERSION
Architecture: $DEB_ARCH
Depends: libc6, libstdc++6
Maintainer: Vision Analysis Project
Description: Pinned Rockchip user-space files for vision analysis
EOF
cat > "$ROCKCHIP_FILES_ROOT/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
ldconfig
EOF
cat > "$ROCKCHIP_FILES_ROOT/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
ldconfig
EOF
chmod 0755 "$ROCKCHIP_FILES_ROOT/DEBIAN/postinst" \
    "$ROCKCHIP_FILES_ROOT/DEBIAN/postrm"
dpkg-deb --build --root-owner-group "$ROCKCHIP_FILES_ROOT" \
    "$APT_DIR/${ROCKCHIP_FILES_PACKAGE}_${DEB_VERSION}_${DEB_ARCH}.deb" >/dev/null

build_meta_package() {
    local package_name="$1"
    local dependency_file="$2"
    shift 2
    local extra_dependencies=("$@")
    local dependencies dependency
    dependencies="$(paste -sd, "$dependency_file" | sed 's/,/, /g')"
    for dependency in "${extra_dependencies[@]}"; do
        [ -n "$dependencies" ] && dependencies+=", "
        dependencies+="$dependency"
    done
    local root="$WORK_DIR/$package_name"
    mkdir -p "$root/DEBIAN"
    cat > "$root/DEBIAN/control" <<EOF
Package: $package_name
Version: $DEB_VERSION
Architecture: $DEB_ARCH
Depends: $dependencies
Maintainer: Vision Analysis Project
Description: Offline dependency metapackage for vision analysis
EOF
    dpkg-deb --build --root-owner-group "$root" \
        "$APT_DIR/${package_name}_${DEB_VERSION}_${DEB_ARCH}.deb" >/dev/null
}

echo ">>> [5/7] 生成项目依赖元包..."
RUNTIME_META="vision-analysis-deps"
build_meta_package "$RUNTIME_META" "$RUNTIME_DIRECT" \
    "$PY_PACKAGE (= $DEB_VERSION)" "$FRONTEND_PACKAGE (= $DEB_VERSION)" \
    "$ROCKCHIP_FILES_PACKAGE (= $DEB_VERSION)"
BUILD_META=""
if [ "$WANT_BUILD" = true ]; then
    BUILD_META="vision-analysis-build-deps"
    build_meta_package "$BUILD_META" "$WORK_DIR/build-only-packages.txt" \
        "$RUNTIME_META (= $DEB_VERSION)"
fi

echo ">>> [6/7] 生成本地 APT 仓库..."
(
    cd "$APT_DIR"
    dpkg-scanpackages -m . /dev/null > Packages 2>/dev/null
    gzip -9c Packages > Packages.gz
)
cat > "$BUNDLE_DIR/BUNDLE_INFO" <<EOF
bundle_format=3
dependency_schema=$OFFLINE_DEPENDENCY_SCHEMA
created_at=$STAMP
os_id=$OS_ID
os_version_id=$OS_VERSION_ID
deb_arch=$DEB_ARCH
python_abi=$PYTHON_ABI
profile=$([ "$WANT_BUILD" = true ] && echo runtime-build || echo runtime)
runtime_meta_package=$RUNTIME_META
build_meta_package=$BUILD_META
package_version=$DEB_VERSION
rockchip_files_package=$ROCKCHIP_FILES_PACKAGE
EOF
cp "$SCRIPT_DIR/install_offline.sh" "$BUNDLE_DIR/install_offline.sh"
chmod +x "$BUNDLE_DIR/install_offline.sh"

# 只验证本地仓库能否完成一次 APT 依赖解析，不检查项目源码和硬件环境。
APT_CHECK_DIR="$WORK_DIR/apt-check"
mkdir -p "$APT_CHECK_DIR/lists/partial" "$APT_CHECK_DIR/sourceparts" \
    "$APT_CHECK_DIR/cache/archives/partial"
: > "$APT_CHECK_DIR/status"
printf 'deb [trusted=yes] file:%s ./\n' "$APT_DIR" > "$APT_CHECK_DIR/sources.list"
APT_CHECK_OPTIONS=(
    -o Debug::NoLocking=true
    -o "APT::Sandbox::User=$(id -un)"
    -o "Dir::Etc::sourcelist=$APT_CHECK_DIR/sources.list"
    -o "Dir::Etc::sourceparts=$APT_CHECK_DIR/sourceparts"
    -o "Dir::State::lists=$APT_CHECK_DIR/lists"
    -o "Dir::State::status=$APT_CHECK_DIR/status"
    -o "Dir::Cache::archives=$APT_CHECK_DIR/cache/archives"
    -o Acquire::Languages=none
)
apt-get "${APT_CHECK_OPTIONS[@]}" update >/dev/null
APT_CHECK_LOG="$WORK_DIR/apt-check.log"
if ! apt-get "${APT_CHECK_OPTIONS[@]}" --simulate --no-install-recommends \
        install "$([ "$WANT_BUILD" = true ] && echo "$BUILD_META" || echo "$RUNTIME_META")" \
        >"$APT_CHECK_LOG" 2>&1; then
    cat "$APT_CHECK_LOG" >&2
    echo "[错误] 组合后的本地 deb 无法完成依赖解析。" >&2
    echo "       可尝试增加缺失包，或使用 --refresh-debs 全量刷新。" >&2
    exit 1
fi

echo ">>> [7/7] 发布离线仓库..."
PREVIOUS="$OUTPUT_DIR/.bundle.previous.$$"
if [ -e "$FINAL_BUNDLE_DIR" ]; then
    mv -- "$FINAL_BUNDLE_DIR" "$PREVIOUS"
fi
if ! mv -- "$BUNDLE_DIR" "$FINAL_BUNDLE_DIR"; then
    [ ! -e "$PREVIOUS" ] || mv -- "$PREVIOUS" "$FINAL_BUNDLE_DIR"
    echo "[错误] 发布失败，已恢复上一版。" >&2
    exit 1
fi
rm -rf -- "$PREVIOUS"

persist_packages() {
    local file="$1"
    shift
    local package
    for package in "$@"; do
        grep -qxF "$package" "$file" 2>/dev/null || printf '%s\n' "$package" >> "$file"
    done
}
persist_packages "$SCRIPT_DIR/extra-runtime-packages.txt" "${ADDED_RUNTIME[@]}"
persist_packages "$SCRIPT_DIR/extra-build-packages.txt" "${ADDED_BUILD[@]}"

BUILD_SUCCEEDED=true
echo
echo "[OK] 新版 deb 离线仓库已生成：$FINAL_BUNDLE_DIR"
echo "     元包: ${RUNTIME_META}_${DEB_VERSION}_${DEB_ARCH}.deb"
[ -z "$BUILD_META" ] || echo "     编译元包: ${BUILD_META}_${DEB_VERSION}_${DEB_ARCH}.deb"
echo "     复制 offline_install_env_debian 后，在目标机执行 install_offline.sh 即可。"
