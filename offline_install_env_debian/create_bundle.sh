#!/usr/bin/env bash
# 在有网 ARM64 开发机上生成可直接由 APT 安装的项目环境与 Web 控制台离线仓库。
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_DIR="${OFFLINE_ENV_DIR:-$SCRIPT_DIR}"
PROJECT_ROOT="${OFFLINE_PROJECT_ROOT:-$(cd "$SCRIPT_DIR/.." && pwd)}"
EXPECTED_OS_ID="${OFFLINE_EXPECTED_OS_ID:-debian}"
ENV_DISPLAY_PATH="${OFFLINE_ENV_DISPLAY_PATH:-offline_install_env_debian}"
STRICT_TARGET_OS="${OFFLINE_STRICT_TARGET_OS:-false}"
DETECTOR="$SCRIPT_DIR/detect_apt_dependencies.sh"
INSTALLER_TEMPLATE="$SCRIPT_DIR/templates/install_offline.sh"
FRONTEND_DIR="$PROJECT_ROOT/web_console/frontend"
VISION_DIR="$PROJECT_ROOT/vision_analysis"
OUTPUT_DIR="$ENV_DIR/output"
SYSTEM_PYTHON="/usr/bin/python3"
FINAL_BUNDLE_DIR=""
REUSE_BUNDLE_DIR=""
LEGACY_BUNDLE_DIR="$OUTPUT_DIR/bundle"
BUNDLE_NAME=""

WANT_BUILD=true
REFRESH_DEBS=false
ADDED_RUNTIME=()
ADDED_BUILD=()
STAGE_DIR=""
APP_BUILD_DIR=""
BUILD_SUCCEEDED=false

usage() {
    printf '用法：bash %s/create_bundle.sh [选项]\n' "$ENV_DISPLAY_PATH"
    cat <<'EOF'

选项：
  --runtime-only          生成精简包，不包含源码、板端编译工具和 Node.js
  --add <APT包名>         添加一个无法自动识别的运行依赖并永久记入清单
  --add-build <APT包名>   添加一个编译依赖并永久记入清单
  --refresh-debs          不复用上一版 deb，按当前软件源全量刷新
  -h, --help              显示帮助

脚本会在制作机临时编译当前应用以识别 ELF 运行依赖，但不会把该程序安装到
目标机的 Web 程序列表。随后自动收集基础清单、extra-*-packages.txt 和
Python requirements，并生成可编译主程序和前端的完整开发仓库。
EOF
}

valid_package_name() {
    [[ "$1" =~ ^[a-z0-9][a-z0-9+.-]+$ ]]
}

while [ "$#" -gt 0 ]; do
    case "$1" in
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

if [ "$WANT_BUILD" = true ]; then
    BUNDLE_NAME="full-bundle"
else
    BUNDLE_NAME="runtime-only-bundle"
fi
FINAL_BUNDLE_DIR="$OUTPUT_DIR/$BUNDLE_NAME"

# shellcheck source=dependency_manifest.sh
source "$ENV_DIR/dependency_manifest.sh"

required_commands=(
    apt-cache apt-get awk cmake curl dpkg dpkg-deb dpkg-query dpkg-scanpackages find grep gzip
    ldconfig make mktemp node npm python3 readelf rsync sed sha256sum sort strings strip tar xargs dpkg-repack
)
for command_name in "${required_commands[@]}"; do
    command -v "$command_name" >/dev/null 2>&1 \
        || { echo "[错误] 制作机缺少命令: $command_name" >&2; exit 1; }
done
[ -x "$SYSTEM_PYTHON" ] && "$SYSTEM_PYTHON" -m pip --version >/dev/null 2>&1 \
    || { echo "[错误] 制作机缺少 /usr/bin/python3 或 python3-pip" >&2; exit 1; }
[ -x "$DETECTOR" ] || chmod +x "$DETECTOR"
[ -f "$INSTALLER_TEMPLATE" ] \
    || { echo "[错误] 缺少安装器模板: $INSTALLER_TEMPLATE" >&2; exit 1; }

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
if [ "$OS_ID" != "$EXPECTED_OS_ID" ]; then
    echo "[错误] $ENV_DISPLAY_PATH 只能在 $EXPECTED_OS_ID 制作机上运行；当前系统为 $OS_ID $OS_VERSION_ID。" >&2
    exit 1
fi
PYTHON_ABI="$($SYSTEM_PYTHON -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
DEB_VERSION="2.0.${STAMP//[TZ]/}"

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
    if [ -n "$APP_BUILD_DIR" ] && [ -d "$APP_BUILD_DIR" ]; then
        rm -rf -- "$APP_BUILD_DIR"
    fi
    if [ -n "$STAGE_DIR" ] && [ -d "$STAGE_DIR" ]; then
        rm -rf -- "$STAGE_DIR"
    fi
    if [ "$BUILD_SUCCEEDED" != true ]; then
        echo "[提示] 制包未完成，原 output/$BUNDLE_NAME 未改变。" >&2
    fi
}
trap cleanup EXIT

mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"
FINAL_BUNDLE_DIR="$OUTPUT_DIR/$BUNDLE_NAME"
LEGACY_BUNDLE_DIR="$OUTPUT_DIR/bundle"
REPACK_CACHE_DIR="$OUTPUT_DIR/repack-cache"
mkdir -p "$REPACK_CACHE_DIR"

# 优先复用相同策略的上一版仓库。首次迁移新目录名时复用旧 output/bundle；
# 另一种策略的仓库也可作为下载缓存，因为仍会按本轮精确包名和版本重新筛选。
REUSE_BUNDLE_DIR="$FINAL_BUNDLE_DIR"
if [ ! -d "$REUSE_BUNDLE_DIR/apt" ]; then
    for reuse_candidate in "$LEGACY_BUNDLE_DIR" \
            "$OUTPUT_DIR/full-bundle" "$OUTPUT_DIR/runtime-only-bundle"; do
        if [ "$reuse_candidate" != "$FINAL_BUNDLE_DIR" ] \
                && [ -d "$reuse_candidate/apt" ]; then
            REUSE_BUNDLE_DIR="$reuse_candidate"
            break
        fi
    done
fi
STAGE_DIR="$(mktemp -d "$OUTPUT_DIR/.offline-build.XXXXXX")"
BUNDLE_DIR="$STAGE_DIR/bundle"
APT_DIR="$BUNDLE_DIR/apt"
WORK_DIR="$STAGE_DIR/work"
mkdir -p "$APT_DIR" "$WORK_DIR"

echo ">>> [1/8] 编译主程序依赖检测产物..."
APP_BUILD_NAME=".offline-deb-app-$STAMP"
APP_BUILD_DIR="$VISION_DIR/$APP_BUILD_NAME"
bash "$VISION_DIR/build.sh" "$APP_BUILD_NAME" \
    --no-bundle-libs --no-root-copy
[ -x "$APP_BUILD_DIR/vision_analysis" ] \
    || { echo "[错误] 项目构建没有生成 vision_analysis。" >&2; exit 1; }

echo ">>> [2/8] 更新索引并识别、下载系统依赖..."
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

# 旧版脚本曾把发行版官方包重封装进缓存，既占空间，也可能把开发机上的
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

if [ -f "$REUSE_BUNDLE_DIR/apt/runtime-direct-packages.txt" ]; then
    NEW_PACKAGES="$(LC_ALL=C comm -13 \
        "$REUSE_BUNDLE_DIR/apt/runtime-direct-packages.txt" "$RUNTIME_DIRECT" || true)"
    if [ -n "$NEW_PACKAGES" ]; then
        echo "    本次新识别的运行依赖："
        printf '%s\n' "$NEW_PACKAGES" | sed 's/^/      + /'
    fi
fi
echo "    直接运行依赖 $(wc -l < "$RUNTIME_DIRECT") 个。"

echo "    收集本地厂商包并下载 $OS_ID 完整依赖闭包..."
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

# 只有 APT 软件源中不存在的本地厂商包才使用 dpkg-repack。发行版官方包
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

    # 把本地包依赖的发行版包加入官方解析；若依赖仍只存在于开发机，
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
    echo "[错误] APT 无法为近乎空白的 $OS_ID 解析官方依赖。" >&2
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
if [ "$REFRESH_DEBS" != true ] && [ -d "$REUSE_BUNDLE_DIR/apt" ]; then
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
    done < <(find "$REUSE_BUNDLE_DIR/apt" -maxdepth 1 -type f -name '*.deb' -print0)
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

echo ">>> [3/8] 将 Python requirements 封装为 deb..."
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
if [ -d "$REUSE_BUNDLE_DIR/python/wheelhouse" ]; then
    cp -a "$REUSE_BUNDLE_DIR/python/wheelhouse/." "$REUSED_WHEELS/"
elif [ -d "$REUSE_BUNDLE_DIR/apt" ]; then
    previous_python_deb="$(find "$REUSE_BUNDLE_DIR/apt" -maxdepth 1 -type f \
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
    if "$SYSTEM_PYTHON" -m pip download --dest "$WHEELHOUSE" \
            --no-index --find-links "$REUSED_WHEELS" \
            --only-binary=:all: --prefer-binary \
            pip setuptools wheel "${PIP_REQUIREMENT_ARGS[@]}"; then
        WHEELS_REUSED=true
        echo "    requirements 未新增缺失项，Python wheels 全部复用。"
    fi
fi
if [ "$WHEELS_REUSED" != true ]; then
    "$SYSTEM_PYTHON" -m pip download --dest "$WHEELHOUSE" \
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
Depends: python3, python3-pip, python3-setuptools, python3-wheel
Maintainer: Vision Analysis Project
Description: System Python dependencies for vision analysis
EOF
cat > "$PY_ROOT/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
share=/usr/share/vision-analysis/python

# pip 默认会跳过已满足版本约束的系统包；只对缺失或版本不符的
# 项目依赖使用随包 wheel 进行安装/替换。Ubuntu 的 PEP 668 环境需要
# --break-system-packages，旧版 pip 则不传该参数。
set --
if /usr/bin/python3 -m pip help install 2>/dev/null | grep -q -- '--break-system-packages'; then
    set -- "$@" --break-system-packages
fi
for requirement in "$share"/requirements/*; do
    [ -f "$requirement" ] || continue
    set -- "$@" -r "$requirement"
done
PIP_ROOT_USER_ACTION=ignore /usr/bin/python3 -m pip install "$@" \
    --no-index --find-links "$share/wheelhouse" --prefer-binary
/usr/bin/python3 -m pip check
# 旧版离线包创建过这个项目专用虚拟环境；新服务已统一使用系统 Python，
# 删除遗留副本，避免人工维护时误用两套环境。
rm -rf -- /opt/vision-analysis/python-env
EOF
chmod 0755 "$PY_ROOT/DEBIAN/postinst"
dpkg-deb --build --root-owner-group "$PY_ROOT" "$APT_DIR/${PY_PACKAGE}_${DEB_VERSION}_${DEB_ARCH}.deb" >/dev/null

echo ">>> [4/8] 构建前端..."
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

CONSOLE_PACKAGE="vision-analysis"
RUNTIME_META="vision-analysis-deps"
BUILD_META=""
SOURCE_PACKAGE=""
SOURCE_INSTALL_PATH=""
NODE_TOOLCHAIN_PACKAGE=""
NODE_TOOLCHAIN_ROOT=""
if [ "$WANT_BUILD" = true ]; then
    BUILD_META="vision-analysis-build-deps"
    SOURCE_PACKAGE="vision-analysis-source"
    SOURCE_INSTALL_PATH="/userdata/rk3588_visual_analysis_framework-$DEB_VERSION"
    NODE_TOOLCHAIN_PACKAGE="vision-analysis-node-toolchain"
    NODE_TOOLCHAIN_ROOT="$NODE_INSTALL_PARENT/node-${DEFAULT_NODE_VERSION}-linux-arm64"
fi

if [ "$WANT_BUILD" = true ]; then
    echo "    封装可离线构建前端的 Node.js/npm 工具链..."
    NODE_DIST="node-${DEFAULT_NODE_VERSION}-linux-arm64"
    NODE_CACHE_DIR="$OUTPUT_DIR/node-cache/$DEFAULT_NODE_VERSION"
    NODE_ARCHIVE="$NODE_CACHE_DIR/$NODE_DIST.tar.xz"
    NODE_CHECKSUMS="$NODE_CACHE_DIR/SHASUMS256.txt"
    NODE_CHECK_LINE="$WORK_DIR/node-package.sha256"
    mkdir -p "$NODE_CACHE_DIR"

    node_archive_is_valid() {
        [ -s "$NODE_ARCHIVE" ] && [ -s "$NODE_CHECKSUMS" ] \
            && grep " ${NODE_DIST}\.tar\.xz$" "$NODE_CHECKSUMS" > "$NODE_CHECK_LINE" \
            && (cd "$NODE_CACHE_DIR" && sha256sum -c "$NODE_CHECK_LINE" >/dev/null 2>&1)
    }

    if node_archive_is_valid; then
        echo "    复用已校验的 Node.js $DEFAULT_NODE_VERSION ARM64 安装包。"
    else
        rm -f -- "$NODE_ARCHIVE" "$NODE_CHECKSUMS"
        NODE_DOWNLOADED=false
        for node_base in \
            "https://registry.npmmirror.com/-/binary/node" \
            "https://mirrors.tuna.tsinghua.edu.cn/nodejs-release" \
            "https://nodejs.org/dist"; do
            echo "    下载并校验: $node_base/$DEFAULT_NODE_VERSION/$NODE_DIST.tar.xz"
            if curl -fSL --connect-timeout 15 --retry 2 \
                    -o "$NODE_ARCHIVE" "$node_base/$DEFAULT_NODE_VERSION/$NODE_DIST.tar.xz" \
                && curl -fSL --connect-timeout 15 --retry 2 \
                    -o "$NODE_CHECKSUMS" "$node_base/$DEFAULT_NODE_VERSION/SHASUMS256.txt" \
                && node_archive_is_valid; then
                NODE_DOWNLOADED=true
                break
            fi
            rm -f -- "$NODE_ARCHIVE" "$NODE_CHECKSUMS"
            echo "    该镜像下载或校验失败，尝试下一个..." >&2
        done
        [ "$NODE_DOWNLOADED" = true ] \
            || { echo "[错误] Node.js ARM64 工具链下载或校验失败。" >&2; exit 1; }
    fi

    NODE_PACKAGE_ROOT="$WORK_DIR/$NODE_TOOLCHAIN_PACKAGE"
    NODE_PACKAGE_DEST="$NODE_PACKAGE_ROOT/$NODE_TOOLCHAIN_ROOT"
    mkdir -p "$NODE_PACKAGE_ROOT/DEBIAN" "$NODE_PACKAGE_DEST" \
        "$NODE_PACKAGE_ROOT/etc/profile.d" "$NODE_PACKAGE_ROOT/usr/local/bin"
    tar -xJf "$NODE_ARCHIVE" -C "$NODE_PACKAGE_DEST" --strip-components=1
    [ -x "$NODE_PACKAGE_DEST/bin/node" ] && [ -x "$NODE_PACKAGE_DEST/bin/npm" ] \
        || { echo "[错误] Node.js 安装包内容不完整。" >&2; exit 1; }
    # 用即将随包交付的同一套 Node/npm 再构建一次，避免制作机当前 PATH 中
    # 的 Node 补丁版本与目标机工具链不同而留下未验证组合。
    (
        cd "$FRONTEND_WORK"
        PATH="$NODE_PACKAGE_DEST/bin:$PATH" "$NODE_PACKAGE_DEST/bin/npm" run build
    )
    cat > "$NODE_PACKAGE_ROOT/etc/profile.d/vision-analysis-node.sh" <<EOF
# 当系统没有可用的 Node.js $MIN_NODE_MAJOR+ 和 npm 时，使用项目离线工具链。
vision_node_major="\$(node -v 2>/dev/null | sed -n 's/^v\([0-9][0-9]*\).*/\1/p')"
if [ -z "\$vision_node_major" ] || [ "\$vision_node_major" -lt $MIN_NODE_MAJOR ] \
        || ! command -v npm >/dev/null 2>&1; then
    export PATH="$NODE_TOOLCHAIN_ROOT/bin:\$PATH"
fi
unset vision_node_major
EOF
    cat > "$NODE_PACKAGE_ROOT/DEBIAN/control" <<EOF
Package: $NODE_TOOLCHAIN_PACKAGE
Version: $DEB_VERSION
Architecture: $DEB_ARCH
Depends: libc6, libstdc++6
Maintainer: Vision Analysis Project
Description: Offline Node.js and npm toolchain for frontend development
EOF
    cat > "$NODE_PACKAGE_ROOT/DEBIAN/postinst" <<EOF
#!/bin/sh
set -e
toolchain='$NODE_TOOLCHAIN_ROOT'
minimum='$MIN_NODE_MAJOR'
major="\$(node -v 2>/dev/null | sed -n 's/^v\([0-9][0-9]*\).*/\1/p')"
if [ -n "\$major" ] && [ "\$major" -ge "\$minimum" ] \
        && command -v npm >/dev/null 2>&1; then
    echo "保留现有 Node.js \$(node -v) / npm \$(npm -v)。"
    exit 0
fi
mkdir -p /usr/local/bin
for executable in node npm npx corepack; do
    target="\$toolchain/bin/\$executable"
    link="/usr/local/bin/\$executable"
    [ -e "\$target" ] || continue
    if [ ! -e "\$link" ] || [ -L "\$link" ]; then
        ln -sfn "\$target" "\$link"
    fi
done
EOF
    cat > "$NODE_PACKAGE_ROOT/DEBIAN/prerm" <<EOF
#!/bin/sh
set -e
toolchain='$NODE_TOOLCHAIN_ROOT'
if [ "\${1:-}" = remove ]; then
    for executable in node npm npx corepack; do
        link="/usr/local/bin/\$executable"
        if [ -L "\$link" ] && [ "\$(readlink "\$link")" = "\$toolchain/bin/\$executable" ]; then
            rm -f "\$link"
        fi
    done
fi
EOF
    chmod 0755 "$NODE_PACKAGE_ROOT/DEBIAN/postinst" \
        "$NODE_PACKAGE_ROOT/DEBIAN/prerm"
    dpkg-deb --build --root-owner-group "$NODE_PACKAGE_ROOT" \
        "$APT_DIR/${NODE_TOOLCHAIN_PACKAGE}_${DEB_VERSION}_${DEB_ARCH}.deb" >/dev/null
fi

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

echo ">>> [5/8] 封装 Web 控制台 deb..."
CONSOLE_ROOT="$WORK_DIR/$CONSOLE_PACKAGE"
CONSOLE_DEST="$CONSOLE_ROOT/opt/ai_apps/_console"
mkdir -p "$CONSOLE_ROOT/DEBIAN" \
    "$CONSOLE_DEST/backend" "$CONSOLE_DEST/frontend" \
    "$CONSOLE_ROOT/lib/systemd/system"

rsync -a --exclude='tests/' --exclude='.pytest_cache/' \
    --exclude='__pycache__/' --exclude='*.pyc' \
    "$PROJECT_ROOT/web_console/backend/" "$CONSOLE_DEST/backend/"
cp -a "$FRONTEND_WORK/dist" "$CONSOLE_DEST/frontend/"
for frontend_asset in logo.png img.png; do
    [ ! -f "$FRONTEND_DIR/$frontend_asset" ] \
        || cp -a "$FRONTEND_DIR/$frontend_asset" "$CONSOLE_DEST/frontend/"
done
if [ -d "$FRONTEND_DIR/logos" ]; then
    cp -a "$FRONTEND_DIR/logos" "$CONSOLE_DEST/frontend/"
fi

cat > "$CONSOLE_ROOT/lib/systemd/system/rk3588-console.service" <<'EOF'
[Unit]
Description=RK3588 Web Config Console
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=root
WorkingDirectory=/opt/ai_apps/_console/backend
Environment="APPS_ROOT=/opt/ai_apps"
Environment="BINARY_NAME=vision_analysis"
Environment="VISION_PYTHON=/usr/bin/python3"
ExecStartPre=-/usr/bin/nm-online -q --timeout=30
ExecStart=/usr/bin/python3 -m uvicorn main:app --host 0.0.0.0 --port 8080 --workers 1 --log-level info
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal
SyslogIdentifier=rk3588-console
KillMode=control-group
KillSignal=SIGTERM
TimeoutStopSec=10

[Install]
WantedBy=multi-user.target
EOF

cat > "$CONSOLE_ROOT/DEBIAN/control" <<EOF
Package: $CONSOLE_PACKAGE
Version: $DEB_VERSION
Architecture: $DEB_ARCH
Depends: $RUNTIME_META (= $DEB_VERSION)
Maintainer: Vision Analysis Project
Description: RK3588 vision analysis Web console
EOF

cat > "$CONSOLE_ROOT/DEBIAN/preinst" <<'EOF'
#!/bin/sh
set -e
# 2.0 以前的同名包曾把默认 C++ 程序装入程序列表。只在跨越 2.0 的升级中
# 清理该旧目录；2.x 之后用户自行安装的同名程序不受后续升级影响。
if [ "${1:-}" = upgrade ] && [ -n "${2:-}" ] \
        && dpkg --compare-versions "$2" lt 2.0; then
    if [ -d /run/systemd/system ]; then
        systemctl stop rk3588-app-vision_analysis-9112f5d54555.service >/dev/null 2>&1 || true
    fi
    rm -rf -- /opt/ai_apps/vision_analysis
fi
EOF
cat > "$CONSOLE_ROOT/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e

# 迁移旧版 web_console/install.sh 直接写入 /etc 的 unit；先留备份，再让
# /lib/systemd/system 中由 deb 管理的 unit 生效。
legacy_unit=/etc/systemd/system/rk3588-console.service
if [ -f "$legacy_unit" ] && grep -q 'SyslogIdentifier=rk3588-console' "$legacy_unit"; then
    backup="${legacy_unit}.pre-deb"
    index=0
    while [ -e "$backup" ]; do
        index=$((index + 1))
        backup="${legacy_unit}.pre-deb.$index"
    done
    mv "$legacy_unit" "$backup"
    echo "已备份旧控制台服务配置: $backup"
fi
legacy_dropin=/etc/systemd/system/rk3588-console.service.d/paths.conf
if [ -f "$legacy_dropin" ]; then
    cp -a "$legacy_dropin" "${legacy_dropin}.pre-deb"
    rm -f "$legacy_dropin"
    rmdir /etc/systemd/system/rk3588-console.service.d 2>/dev/null || true
fi

mkdir -p /opt/ai_apps/.data
if [ -d /run/systemd/system ]; then
    systemctl daemon-reload
    # 旧安装创建的 wants 链接可能仍指向已经备份的 /etc unit。reenable 会把
    # 链接统一重建到当前由 deb 管理的 /lib unit，确保重启设备后仍能启动。
    systemctl reenable rk3588-console.service >/dev/null 2>&1 \
        || systemctl enable rk3588-console.service >/dev/null 2>&1 \
        || true
    if ! systemctl restart rk3588-console.service; then
        echo "[警告] Web 控制台暂未启动；可运行 systemctl status rk3588-console 查看原因。" >&2
    fi
fi
EOF
cat > "$CONSOLE_ROOT/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e
if [ "${1:-}" = remove ] && [ -d /run/systemd/system ]; then
    systemctl disable --now rk3588-console.service >/dev/null 2>&1 || true
fi
EOF
cat > "$CONSOLE_ROOT/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
if [ -d /run/systemd/system ]; then
    systemctl daemon-reload
fi
EOF
chmod 0755 "$CONSOLE_ROOT/DEBIAN/preinst" "$CONSOLE_ROOT/DEBIAN/postinst" \
    "$CONSOLE_ROOT/DEBIAN/prerm" "$CONSOLE_ROOT/DEBIAN/postrm"
dpkg-deb --build --root-owner-group "$CONSOLE_ROOT" \
    "$APT_DIR/${CONSOLE_PACKAGE}_${DEB_VERSION}_${DEB_ARCH}.deb" >/dev/null

if [ "$WANT_BUILD" = true ]; then
    echo "    封装可在板端编译的项目源码 deb..."
    SOURCE_TREE="$WORK_DIR/source-tree"
    SOURCE_ROOT="$WORK_DIR/$SOURCE_PACKAGE"
    SOURCE_SHARE="$SOURCE_ROOT/usr/share/vision-analysis/source"
    mkdir -p "$SOURCE_TREE" "$SOURCE_ROOT/DEBIAN" "$SOURCE_SHARE"

    # Git 工作区按已跟踪文件打包，既包含当前未提交的源码修改，也不会混入
    # output、build、node_modules 和个人测试素材。下载的源码压缩包没有 .git
    # 时使用同等的显式排除规则。
    if command -v git >/dev/null 2>&1 \
            && git -C "$PROJECT_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        (
            cd "$PROJECT_ROOT"
            while IFS= read -r -d '' source_path; do
                [ "$source_path" = 'vision_analysis/vision_analysis' ] && continue
                [ "$source_path" = 'first_net_config/first_net_config' ] && continue
                [ -e "$source_path" ] && printf '%s\0' "$source_path"
            done < <(git ls-files -z --cached --others --exclude-standard)
        ) | rsync -a --from0 --files-from=- "$PROJECT_ROOT/" "$SOURCE_TREE/"
    else
        rsync -a \
            --exclude='.git/' --exclude='.claude/' --exclude='.vscode/' \
            --exclude='.pytest_cache/' --exclude='__pycache__/' --exclude='*.pyc' \
            --exclude='build/' --exclude='dist/' --exclude='node_modules/' \
            --exclude='offline_install_env_debian/output/' \
            --exclude='offline_install_env_ubuntu/output/' \
            --exclude='vision_analysis/vision_analysis' \
            --exclude='first_net_config/first_net_config' \
            --exclude='*.mp4' --exclude='*.avi' --exclude='*.mkv' --exclude='*.docx' \
            "$PROJECT_ROOT/" "$SOURCE_TREE/"
    fi
    [ -f "$SOURCE_TREE/vision_analysis/CMakeLists.txt" ] \
        || { echo "[错误] 源码包缺少 vision_analysis/CMakeLists.txt。" >&2; exit 1; }
    [ -f "$SOURCE_TREE/vision_analysis/build.sh" ] \
        || { echo "[错误] 源码包缺少 vision_analysis/build.sh。" >&2; exit 1; }
    [ -d "$FRONTEND_WORK/node_modules" ] \
        || { echo "[错误] 前端依赖目录 node_modules 不存在。" >&2; exit 1; }
    tar -C "$SOURCE_TREE" -czf "$SOURCE_SHARE/source.tar.gz" .
    # node_modules 以单个归档随源码 deb 携带，避免制包暂存区复制数千个文件
    # 耗尽 inode；目标机 postinst 会自动还原为普通 node_modules 目录。
    tar -C "$FRONTEND_WORK" -czf "$SOURCE_SHARE/frontend-node-modules.tar.gz" \
        node_modules
    cat > "$SOURCE_SHARE/README.txt" <<EOF
项目源码由 $SOURCE_PACKAGE $DEB_VERSION 安装。
实际目录: $SOURCE_INSTALL_PATH
固定入口: /userdata/rk3588_visual_analysis_framework
编译命令:
  cd /userdata/rk3588_visual_analysis_framework/vision_analysis
  ./build.sh dist

前端构建命令（Node.js、npm 和 node_modules 已离线提供）:
  cd /userdata/rk3588_visual_analysis_framework/web_console/frontend
  PATH=$NODE_TOOLCHAIN_ROOT/bin:\$PATH npm run build

升级会创建新的版本目录并更新固定入口，不会删除旧版本目录中的板端修改。
EOF
    cat > "$SOURCE_ROOT/DEBIAN/control" <<EOF
Package: $SOURCE_PACKAGE
Version: $DEB_VERSION
Architecture: $DEB_ARCH
Depends: tar, $BUILD_META (= $DEB_VERSION)
Maintainer: Vision Analysis Project
Description: Buildable source tree for RK3588 vision analysis
EOF
    cat > "$SOURCE_ROOT/DEBIAN/postinst" <<EOF
#!/bin/sh
set -e
version_dir='$SOURCE_INSTALL_PATH'
stable_path='/userdata/rk3588_visual_analysis_framework'
archive='/usr/share/vision-analysis/source/source.tar.gz'
frontend_modules_archive='/usr/share/vision-analysis/source/frontend-node-modules.tar.gz'

mkdir -p /userdata
if [ ! -d "\$version_dir" ]; then
    stage="\${version_dir}.new.\$\$"
    rm -rf "\$stage"
    mkdir -p "\$stage"
    tar -xzf "\$archive" -C "\$stage"
    printf '%s\n' '$DEB_VERSION' > "\$stage/.vision-analysis-source-version"
    mv "\$stage" "\$version_dir"
fi

frontend_dir="\$version_dir/web_console/frontend"
if [ ! -d "\$frontend_dir/node_modules" ]; then
    [ -f "\$frontend_modules_archive" ] \
        || { echo "[错误] 源码包缺少前端依赖归档。" >&2; exit 1; }
    mkdir -p "\$frontend_dir"
    tar -xzf "\$frontend_modules_archive" -C "\$frontend_dir"
fi

if [ -L "\$stable_path" ] || [ ! -e "\$stable_path" ]; then
    ln -sfn "\$version_dir" "\$stable_path"
else
    echo "[提示] \$stable_path 已是现有目录，未覆盖它。" >&2
    echo "       本版源码位于: \$version_dir" >&2
fi
EOF
    cat > "$SOURCE_ROOT/DEBIAN/prerm" <<EOF
#!/bin/sh
set -e
version_dir='$SOURCE_INSTALL_PATH'
stable_path='/userdata/rk3588_visual_analysis_framework'
if [ "\${1:-}" = remove ] && [ -L "\$stable_path" ] \
        && [ "\$(readlink "\$stable_path")" = "\$version_dir" ]; then
    rm -f "\$stable_path"
fi
EOF
    chmod 0755 "$SOURCE_ROOT/DEBIAN/postinst" "$SOURCE_ROOT/DEBIAN/prerm"
    dpkg-deb --build --root-owner-group "$SOURCE_ROOT" \
        "$APT_DIR/${SOURCE_PACKAGE}_${DEB_VERSION}_${DEB_ARCH}.deb" >/dev/null
fi

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

echo ">>> [6/8] 生成内部依赖元包..."
build_meta_package "$RUNTIME_META" "$RUNTIME_DIRECT" \
    "$PY_PACKAGE (= $DEB_VERSION)" \
    "$ROCKCHIP_FILES_PACKAGE (= $DEB_VERSION)"
if [ "$WANT_BUILD" = true ]; then
    build_meta_package "$BUILD_META" "$WORK_DIR/build-only-packages.txt" \
        "$RUNTIME_META (= $DEB_VERSION)" \
        "$NODE_TOOLCHAIN_PACKAGE (= $DEB_VERSION)"
fi

echo ">>> [7/8] 生成本地 APT 仓库..."
(
    cd "$APT_DIR"
    dpkg-scanpackages -m . /dev/null > Packages 2>/dev/null
    gzip -9c Packages > Packages.gz
)
cat > "$BUNDLE_DIR/BUNDLE_INFO" <<EOF
bundle_format=10
dependency_schema=$OFFLINE_DEPENDENCY_SCHEMA
created_at=$STAMP
os_id=$OS_ID
os_version_id=$OS_VERSION_ID
deb_arch=$DEB_ARCH
python_abi=$PYTHON_ABI
profile=$([ "$WANT_BUILD" = true ] && echo runtime-build || echo runtime)
bundle_name=$BUNDLE_NAME
strict_target_os=$STRICT_TARGET_OS
runtime_meta_package=$RUNTIME_META
build_meta_package=$BUILD_META
package_version=$DEB_VERSION
python_package=$PY_PACKAGE
rockchip_files_package=$ROCKCHIP_FILES_PACKAGE
console_package=$CONSOLE_PACKAGE
source_package=$SOURCE_PACKAGE
source_install_path=$SOURCE_INSTALL_PATH
node_toolchain_package=$NODE_TOOLCHAIN_PACKAGE
node_toolchain_root=$NODE_TOOLCHAIN_ROOT
EOF
cp "$INSTALLER_TEMPLATE" "$BUNDLE_DIR/install_offline.sh"
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
APT_CHECK_TARGETS=("$CONSOLE_PACKAGE")
if [ "$WANT_BUILD" = true ]; then
    APT_CHECK_TARGETS+=("$BUILD_META" "$SOURCE_PACKAGE")
fi
if ! apt-get "${APT_CHECK_OPTIONS[@]}" --simulate --no-install-recommends \
        install "${APT_CHECK_TARGETS[@]}" \
        >"$APT_CHECK_LOG" 2>&1; then
    cat "$APT_CHECK_LOG" >&2
    echo "[错误] 组合后的本地 deb 无法完成依赖解析。" >&2
    echo "       可尝试增加缺失包，或使用 --refresh-debs 全量刷新。" >&2
    exit 1
fi

echo ">>> [8/8] 发布离线仓库..."
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

# 新命名完成发布后删除旧的通用 bundle，避免用户分不清该使用哪个策略。
if [ -d "$LEGACY_BUNDLE_DIR" ]; then
    rm -rf -- "$LEGACY_BUNDLE_DIR"
fi

persist_packages() {
    local file="$1"
    shift
    local package
    for package in "$@"; do
        grep -qxF "$package" "$file" 2>/dev/null || printf '%s\n' "$package" >> "$file"
    done
}
persist_packages "$ENV_DIR/extra-runtime-packages.txt" "${ADDED_RUNTIME[@]}"
persist_packages "$ENV_DIR/extra-build-packages.txt" "${ADDED_BUILD[@]}"

BUILD_SUCCEEDED=true
echo
echo "[OK] 新版 deb 离线仓库已生成：$FINAL_BUNDLE_DIR"
echo "     控制台包: ${CONSOLE_PACKAGE}_${DEB_VERSION}_${DEB_ARCH}.deb"
echo "     不预装默认 C++ 程序；Web 程序列表在首次安装后为空。"
if [ -n "$BUILD_META" ]; then
    echo "     默认同时安装 C/C++、Node.js/npm、前端 node_modules 和项目源码。"
    echo "     源码入口: /userdata/rk3588_visual_analysis_framework"
fi
echo "     复制 output/$BUNDLE_NAME 到目标机后，在包目录内执行 install_offline.sh。"
