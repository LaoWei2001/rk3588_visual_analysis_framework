#!/usr/bin/env bash
# 为 Ubuntu 离线包输出项目直接使用的 APT 包名：基础清单、用户追加项和
# 已构建 ELF 的动态库归属。本脚本不调用 Debian 离线环境中的任何文件。
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_DIR="$SCRIPT_DIR"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WANT_BUILD="${OFFLINE_DETECT_BUILD_DEPS:-false}"
[ "$#" -eq 0 ] || { echo "[错误] detect_apt_dependencies.sh 无需命令行参数。" >&2; exit 2; }

# shellcheck source=dependency_manifest.sh
source "$ENV_DIR/dependency_manifest.sh"

# shellcheck source=/etc/os-release
source /etc/os-release
if [ "${ID:-unknown}" != ubuntu ]; then
    echo "[错误] Ubuntu 依赖探测器不能在 ${ID:-unknown} ${VERSION_ID:-unknown} 上运行。" >&2
    exit 1
fi
if [ "$(uname -m)" != aarch64 ] || [ "$(dpkg --print-architecture)" != arm64 ]; then
    echo "[错误] Ubuntu 依赖探测器要求 ARM64；当前 uname=$(uname -m), dpkg=$(dpkg --print-architecture)。" >&2
    exit 1
fi

declare -A PACKAGES=()
declare -A SCANNED_LIBRARY_PATHS=()
declare -A LIBRARY_PATHS=()
declare -A AUTO_PACKAGES=()

add_package() {
    local package="${1%%:*}"
    [[ "$package" =~ ^[a-z0-9][a-z0-9+.-]+$ ]] || return 0
    PACKAGES["$package"]=1
}

read_extra_file() {
    local file="$1"
    local package
    [ -f "$file" ] || return 0
    while IFS= read -r package; do
        package="${package%%#*}"
        package="$(xargs <<< "$package")"
        [ -n "$package" ] && add_package "$package"
    done < "$file"
    return 0
}

package_owning_path() {
    local path="$1"
    local resolved owner
    if [ -n "${SCANNED_LIBRARY_PATHS[$path]+set}" ]; then
        return 0
    fi
    SCANNED_LIBRARY_PATHS["$path"]=1
    for resolved in "$path" "$(readlink -f "$path" 2>/dev/null || true)"; do
        [ -n "$resolved" ] || continue
        owner="$(dpkg-query -S "$resolved" 2>/dev/null | sed -n '1s/: .*//p' || true)"
        owner="${owner%%:*}"
        if [ -n "$owner" ]; then
            AUTO_PACKAGES["$owner"]=1
            return
        fi
    done
    return 0
}

for package in "${APT_RUNTIME[@]}"; do
    add_package "$package"
done
for package in "${LOCAL_RUNTIME_PACKAGES[@]}"; do
    add_package "$package"
done
read_extra_file "$ENV_DIR/extra-runtime-packages.txt"

if [ "$WANT_BUILD" = true ]; then
    for package in "${APT_BUILD[@]}"; do
        add_package "$package"
    done
    for package in "${LOCAL_BUILD_PACKAGES[@]}"; do
        add_package "$package"
    done
    read_extra_file "$ENV_DIR/extra-build-packages.txt"
fi

# 读取动态链接器缓存一次，后续按 ELF 的直接 NEEDED 项查询，避免对每个二进制运行
# ldd 并把整个传递依赖树误当成项目直接依赖。
while read -r soname library_path; do
    [ -n "$soname" ] && [ -n "$library_path" ] || continue
    if [ -z "${LIBRARY_PATHS[$soname]+set}" ]; then
        LIBRARY_PATHS["$soname"]="$library_path"
    fi
done < <(ldconfig -p 2>/dev/null | awk '/aarch64|AArch64/ {print $1, $NF}')

# 新增 C/C++ 动态库并完成一次构建后，可执行文件的 NEEDED 库会在这里自动映射
# 回提供该库的发行版包。排除离线包、Git 与前端缓存，避免扫描生成物仓库。
while IFS= read -r -d '' candidate; do
    readelf -h "$candidate" >/dev/null 2>&1 || continue
    while IFS= read -r soname; do
        [ -n "${LIBRARY_PATHS[$soname]:-}" ] \
            && package_owning_path "${LIBRARY_PATHS[$soname]}"
    done < <(
        LC_ALL=C readelf -d "$candidate" 2>/dev/null \
            | sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p' \
            | LC_ALL=C sort -u
    )
done < <(
    find "$PROJECT_ROOT" \
        \( -path "$PROJECT_ROOT/.git" \
           -o -path "$ENV_DIR/output" \
           -o -path '*/node_modules' \
           -o -path '*/.venv' \) -prune \
        -o -type f \( -perm /111 -o -name '*.so' -o -name '*.so.*' \) -print0
)

# 制作完整包时，再从编译器生成的 .d 文件识别实际使用过的 /usr/include 头文件归属。
# 新增开发库后先正常构建一次，下一次制包即可自动发现其 -dev 包。
if [ "$WANT_BUILD" = true ]; then
    while IFS= read -r dependency_file; do
        while IFS= read -r header; do
            [ -f "$header" ] && package_owning_path "$header"
        done < <(tr '\\' ' ' < "$dependency_file" \
            | grep -oE '/usr/include/[^[:space:]]+' \
            | LC_ALL=C sort -u || true)
    done < <(find "$PROJECT_ROOT" -type f -name '*.d' \
        ! -path "$ENV_DIR/output/*" ! -path '*/node_modules/*')
fi

# 自动识别结果无论是否存在于软件源都输出。制包器会把它们分类为：
# 可从 APT 精确下载的官方包，或必须从开发机严格重封装的本地厂商包。
AUTO_PACKAGE_LIST="$(printf '%s\n' "${!AUTO_PACKAGES[@]}" | LC_ALL=C sort -u)"
while IFS= read -r package; do
    [ -n "$package" ] && add_package "$package"
done < <(printf '%s\n' "$AUTO_PACKAGE_LIST")

printf '%s\n' "${!PACKAGES[@]}" | LC_ALL=C sort
