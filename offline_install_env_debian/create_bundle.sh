#!/usr/bin/env bash
# 在有公网的、与现场系统一致的 RK3588 上制作完整离线依赖包。
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEPENDENCY_MANIFEST="$SCRIPT_DIR/dependency_manifest.sh"
FRONTEND_DIR="$PROJECT_ROOT/web_console/frontend"

WANT_BUILD=false
SKIP_APT_UPDATE=false
OUTPUT_DIR="$SCRIPT_DIR/output"
FINAL_BUNDLE_DIR="$OUTPUT_DIR/bundle"
STAGE_DIR=""
BUILD_SUCCEEDED=false

usage() {
    cat <<'EOF'
用法：bash offline_install_env_debian/create_bundle.sh [选项]

选项：
  --build              同时收集 RK3588 板端 C/C++ 编译环境
  --skip-apt-update    不刷新 APT 索引，仅使用制作机现有索引
  -h, --help           显示帮助

必须在有公网的 ARM64 RK3588 上执行；制作机与现场机必须使用相同的
发行版和版本。脚本不会安装或升级系统包，只会生成 output/bundle。
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build) WANT_BUILD=true ;;
        --skip-apt-update) SKIP_APT_UPDATE=true ;;
        -h|--help) usage; exit 0 ;;
        *) echo "[错误] 未知参数: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

[ -f "$DEPENDENCY_MANIFEST" ] \
    || { echo "[错误] 缺少依赖清单: $DEPENDENCY_MANIFEST" >&2; exit 1; }
# shellcheck source=dependency_manifest.sh
source "$DEPENDENCY_MANIFEST"

required_commands=(
    apt-get awk curl date df dpkg dpkg-scanpackages du find gzip id mktemp
    python3 sed sha256sum sort tar wc xargs xz
)
for command_name in "${required_commands[@]}"; do
    command -v "$command_name" >/dev/null 2>&1 \
        || { echo "[错误] 制作机缺少命令: $command_name" >&2; exit 1; }
done
python3 -m pip --version >/dev/null 2>&1 \
    || { echo "[错误] 制作机缺少 python3-pip" >&2; exit 1; }

MACHINE_ARCH="$(uname -m)"
DEB_ARCH="$(dpkg --print-architecture)"
if [ "$MACHINE_ARCH" != "aarch64" ] || [ "$DEB_ARCH" != "arm64" ]; then
    echo "[错误] 离线包必须在 ARM64 RK3588 上制作。" >&2
    echo "       当前 uname=$MACHINE_ARCH, dpkg=$DEB_ARCH" >&2
    echo "       x86 主机下载的 Python/npm 原生模块不能在 RK3588 使用。" >&2
    exit 1
fi

# /etc/os-release 由系统提供，仅用于写入和校验平台元数据。
# shellcheck source=/etc/os-release
source /etc/os-release
OS_ID="${ID:-unknown}"
OS_VERSION_ID="${VERSION_ID:-unknown}"
OS_CODENAME="${VERSION_CODENAME:-unknown}"
PYTHON_ABI="$(python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')"
NODE_VERSION="${NODE_VERSION:-$DEFAULT_NODE_VERSION}"
if [[ ! "$NODE_VERSION" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "[错误] NODE_VERSION 格式无效: $NODE_VERSION" >&2
    exit 2
fi

for metadata_value in "$OS_ID" "$OS_VERSION_ID" "$OS_CODENAME" "$PYTHON_ABI"; do
    if [[ ! "$metadata_value" =~ ^[A-Za-z0-9._+-]+$ ]]; then
        echo "[错误] 平台元数据包含不支持的字符: $metadata_value" >&2
        exit 1
    fi
done

if [ "$(id -u)" -eq 0 ]; then
    ROOT=()
elif command -v sudo >/dev/null 2>&1; then
    ROOT=(sudo)
else
    echo "[错误] 刷新 APT 索引和下载软件包需要 root 或 sudo。" >&2
    exit 1
fi

as_root() {
    "${ROOT[@]}" "$@"
}

cleanup() {
    if [ -n "$STAGE_DIR" ] && [ -d "$STAGE_DIR" ]; then
        rm -rf -- "$STAGE_DIR"
        if [ "$BUILD_SUCCEEDED" != true ]; then
            echo "[提示] 制包未完成，已清理不完整的临时材料；原 output/bundle 未改变。" >&2
        fi
    fi
}
trap cleanup EXIT

mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"
if [[ "$OUTPUT_DIR" =~ [[:space:]] ]]; then
    echo "[错误] 输出目录路径不能包含空格，否则现场 APT file: 仓库无法可靠识别: $OUTPUT_DIR" >&2
    exit 1
fi
AVAILABLE_KB="$(df -Pk "$OUTPUT_DIR" | awk 'NR == 2 {print $4}')"
MINIMUM_KB=$((1024 * 1024))
if [ "${AVAILABLE_KB:-0}" -lt "$MINIMUM_KB" ]; then
    echo "[错误] 项目所在文件系统可用空间不足 1 GiB: $OUTPUT_DIR" >&2
    echo "       当前约 $(( ${AVAILABLE_KB:-0} / 1024 )) MiB；请先释放空间。" >&2
    exit 1
fi
PROFILE="runtime"
[ "$WANT_BUILD" = true ] && PROFILE="runtime-build"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
BUNDLE_NAME="rk3588-deps-${OS_ID}${OS_VERSION_ID}-${DEB_ARCH}-${PROFILE}-${STAMP}"
STAGE_DIR="$(mktemp -d "$OUTPUT_DIR/.offline-build.XXXXXX")"
BUNDLE_DIR="$STAGE_DIR/bundle"
mkdir -p "$BUNDLE_DIR"/{apt,python/wheelhouse,python/requirements,node,frontend,project}

echo "============================================================"
echo "  RK3588 项目离线依赖包制作"
echo "  平台: $OS_ID $OS_VERSION_ID ($OS_CODENAME), $DEB_ARCH"
echo "  Python: $PYTHON_ABI    Node: $NODE_VERSION"
echo "  内容: $PROFILE"
echo "  输出: $FINAL_BUNDLE_DIR"
echo "  说明: 只下载和临时构建，不会安装/升级制作机上的软件包"
echo "============================================================"

if [ "$SKIP_APT_UPDATE" != true ]; then
    echo ">>> [1/6] 刷新 APT 索引..."
    if ! as_root apt-get update; then
        echo "    [警告] 部分软件源刷新失败；将继续验证现有索引能否解析全部依赖。" >&2
    fi
else
    echo ">>> [1/6] 跳过 APT 索引刷新，使用现有索引。"
fi

echo ">>> [2/6] 下载 APT 完整依赖闭包..."
APT_PACKAGES=("${APT_RUNTIME[@]}")
printf '%s\n' "${APT_RUNTIME[@]}" > "$BUNDLE_DIR/apt/runtime-packages.txt"
: > "$BUNDLE_DIR/apt/build-packages.txt"
if [ "$WANT_BUILD" = true ]; then
    APT_PACKAGES+=("${APT_BUILD[@]}")
    printf '%s\n' "${APT_BUILD[@]}" > "$BUNDLE_DIR/apt/build-packages.txt"
fi

# 使用空 dpkg 状态让 APT 解析器列出 Depends/Pre-Depends 的完整闭包，而不是只处理
# 制作机当前缺少的包。随后分批执行 apt-get download：每个 deb 都由 APT 按已签名
# 索引校验，某一批网络失败时也不会丢掉此前已经完成的数百个文件。
EMPTY_DPKG_STATUS="$STAGE_DIR/empty-dpkg-status"
: > "$EMPTY_DPKG_STATUS"
chmod 755 "$STAGE_DIR" "$BUNDLE_DIR" "$BUNDLE_DIR/apt"
apt-get --simulate \
    -o Debug::NoLocking=true \
    -o "Dir::State::status=$EMPTY_DPKG_STATUS" \
    --no-install-recommends install "${APT_PACKAGES[@]}" \
    | awk '$1 == "Inst" {version=$3; sub(/^\(/, "", version); print $2 "=" version}' \
    > "$BUNDLE_DIR/apt/resolved-packages.txt"
mapfile -t APT_RESOLVED_SPECS < "$BUNDLE_DIR/apt/resolved-packages.txt"
RESOLVED_COUNT="${#APT_RESOLVED_SPECS[@]}"
[ "$RESOLVED_COUNT" -gt 0 ] || { echo "[错误] APT 未解析出依赖闭包" >&2; exit 1; }

APT_BATCH_SIZE=20
for ((batch_start=0; batch_start<RESOLVED_COUNT; batch_start+=APT_BATCH_SIZE)); do
    APT_BATCH=("${APT_RESOLVED_SPECS[@]:batch_start:APT_BATCH_SIZE}")
    APT_BATCH_OK=false
    for apt_attempt in 1 2 3; do
        if (
            cd "$BUNDLE_DIR/apt"
            apt-get download -qq \
                -o "APT::Sandbox::User=$(id -un)" \
                -o Acquire::Retries=3 \
                -o Acquire::http::Timeout=30 \
                -o Acquire::https::Timeout=30 \
                "${APT_BATCH[@]}"
        ); then
            APT_BATCH_OK=true
            break
        fi
        echo "    [警告] 第 $((batch_start / APT_BATCH_SIZE + 1)) 批下载失败（尝试 $apt_attempt/3），保留已有文件后重试。" >&2
    done
    [ "$APT_BATCH_OK" = true ] \
        || { echo "[错误] APT 第 $((batch_start / APT_BATCH_SIZE + 1)) 批在三轮重试后仍不完整" >&2; exit 1; }
    completed=$((batch_start + ${#APT_BATCH[@]}))
    echo "    APT: $completed/$RESOLVED_COUNT"
done
DEB_COUNT="$(find "$BUNDLE_DIR/apt" -maxdepth 1 -type f -name '*.deb' | wc -l)"
[ "$DEB_COUNT" -gt 0 ] || { echo "[错误] APT 阶段没有下载到 deb 文件" >&2; exit 1; }
[ "$DEB_COUNT" -eq "$RESOLVED_COUNT" ] \
    || { echo "[错误] APT 应有 $RESOLVED_COUNT 个 deb，实际只有 $DEB_COUNT 个" >&2; exit 1; }
(
    cd "$BUNDLE_DIR/apt"
    dpkg-scanpackages -m . /dev/null > Packages
    gzip -9c Packages > Packages.gz
)
echo "    已收集 $DEB_COUNT 个 deb 软件包。"

echo ">>> [3/6] 下载并校验 Node.js ARM64 发布包..."
NODE_PACKAGE="node-${NODE_VERSION}-linux-arm64"
NODE_ARCHIVE="$BUNDLE_DIR/node/$NODE_PACKAGE.tar.xz"
NODE_CHECKSUMS="$STAGE_DIR/SHASUMS256.txt"
NODE_VERIFIED=false
for base_url in \
    "https://registry.npmmirror.com/-/binary/node" \
    "https://mirrors.tuna.tsinghua.edu.cn/nodejs-release" \
    "https://nodejs.org/dist"; do
    echo "    尝试: $base_url/$NODE_VERSION/$NODE_PACKAGE.tar.xz"
    if curl -fSL --connect-timeout 15 --retry 2 -o "$NODE_ARCHIVE" \
            "$base_url/$NODE_VERSION/$NODE_PACKAGE.tar.xz" \
        && curl -fSL --connect-timeout 15 --retry 2 -o "$NODE_CHECKSUMS" \
            "$base_url/$NODE_VERSION/SHASUMS256.txt" \
        && grep " ${NODE_PACKAGE}\.tar\.xz$" "$NODE_CHECKSUMS" \
            > "$BUNDLE_DIR/node/UPSTREAM_SHA256SUMS" \
        && (cd "$BUNDLE_DIR/node" && sha256sum -c UPSTREAM_SHA256SUMS); then
        NODE_VERIFIED=true
        break
    fi
done
[ "$NODE_VERIFIED" = true ] \
    || { echo "[错误] Node.js 发布包下载或上游 SHA-256 校验失败" >&2; exit 1; }

NODE_EXTRACT_DIR="$STAGE_DIR/node-runtime"
mkdir -p "$NODE_EXTRACT_DIR"
tar -xJf "$NODE_ARCHIVE" -C "$NODE_EXTRACT_DIR"
export PATH="$NODE_EXTRACT_DIR/$NODE_PACKAGE/bin:$PATH"
hash -r 2>/dev/null || true
[ "$(node -v)" = "$NODE_VERSION" ] \
    || { echo "[错误] 下载的 Node.js 无法在制作机运行" >&2; exit 1; }

echo ">>> [4/6] 下载 Python ARM64 wheel 及递归依赖..."
PIP_REQUIREMENT_ARGS=()
: > "$BUNDLE_DIR/python/requirements.list"
for requirement_relative in "${PYTHON_REQUIREMENTS[@]}"; do
    requirement_source="$PROJECT_ROOT/$requirement_relative"
    [ -f "$requirement_source" ] \
        || { echo "[错误] requirements 文件不存在: $requirement_relative" >&2; exit 1; }
    requirement_bundle_name="${requirement_relative//\//__}"
    cp "$requirement_source" "$BUNDLE_DIR/python/requirements/$requirement_bundle_name"
    printf '%s\n' "$requirement_bundle_name" >> "$BUNDLE_DIR/python/requirements.list"
    PIP_REQUIREMENT_ARGS+=(-r "$requirement_source")
done
python3 -m pip download \
    --dest "$BUNDLE_DIR/python/wheelhouse" \
    --only-binary=:all: --prefer-binary \
    pip setuptools wheel "${PIP_REQUIREMENT_ARGS[@]}"
WHEEL_COUNT="$(find "$BUNDLE_DIR/python/wheelhouse" -maxdepth 1 -type f | wc -l)"
[ "$WHEEL_COUNT" -gt 0 ] || { echo "[错误] Python wheelhouse 为空" >&2; exit 1; }
echo "    已收集 $WHEEL_COUNT 个 Python wheel。"

echo ">>> [5/6] 锁定安装并构建 Web 前端..."
[ -f "$FRONTEND_DIR/package-lock.json" ] \
    || { echo "[错误] 缺少 $FRONTEND_DIR/package-lock.json" >&2; exit 1; }
FRONTEND_WORK="$STAGE_DIR/frontend-work"
mkdir -p "$FRONTEND_WORK"
(
    cd "$FRONTEND_DIR"
    tar --exclude='./node_modules' --exclude='./dist' --exclude='*.tsbuildinfo' -cf - .
) | (
    cd "$FRONTEND_WORK"
    tar -xf -
)
(
    cd "$FRONTEND_WORK"
    npm ci --no-audit --no-fund
    npm run build
)
[ -f "$FRONTEND_WORK/dist/index.html" ] \
    || { echo "[错误] 前端构建未生成 dist/index.html" >&2; exit 1; }
NPM_TREE_CHECK=""
if ! NPM_TREE_CHECK="$(cd "$FRONTEND_WORK" && npm ls --depth=0 2>&1)"; then
    echo "[错误] npm ci 完成后依赖树仍与 package-lock.json 不一致：" >&2
    printf '%s\n' "$NPM_TREE_CHECK" | sed -n '1,20s/^/       /p' >&2
    exit 1
fi
tar -C "$FRONTEND_WORK" -czf "$BUNDLE_DIR/frontend/node_modules.tar.gz" node_modules
tar -C "$FRONTEND_WORK" -czf "$BUNDLE_DIR/frontend/dist.tar.gz" dist
cp "$FRONTEND_WORK/package.json" "$FRONTEND_WORK/package-lock.json" "$BUNDLE_DIR/frontend/"

# 安装前先核对项目输入，防止把旧离线包用于已改变依赖或前端源码的新项目。
for project_file in "${OFFLINE_PROJECT_FILES[@]}"; do
    [ -f "$PROJECT_ROOT/$project_file" ] \
        || { echo "[错误] 项目校验文件不存在: $project_file" >&2; exit 1; }
done
(
    cd "$PROJECT_ROOT"
    sha256sum "${OFFLINE_PROJECT_FILES[@]}" "${PYTHON_REQUIREMENTS[@]}"
) > "$BUNDLE_DIR/project/REQUIREMENTS_SHA256SUMS"
(
    cd "$FRONTEND_DIR"
    find . -type f \
        ! -path './node_modules/*' ! -path './dist/*' \
        ! -path './logos/*' \
        ! -name 'logo.png' ! -name 'img.png' \
        ! -name '*.tsbuildinfo' \
        -print0 | LC_ALL=C sort -z | xargs -0 sha256sum
) > "$BUNDLE_DIR/project/FRONTEND_SHA256SUMS"

echo ">>> [6/6] 写入平台信息、完整性清单并发布..."
PROJECT_COMMIT="unknown"
if command -v git >/dev/null 2>&1 && git -C "$PROJECT_ROOT" rev-parse --verify HEAD >/dev/null 2>&1; then
    PROJECT_COMMIT="$(git -C "$PROJECT_ROOT" rev-parse HEAD)"
fi
cat > "$BUNDLE_DIR/BUNDLE_INFO" <<EOF
bundle_format=1
dependency_schema=$OFFLINE_DEPENDENCY_SCHEMA
created_at=$STAMP
profile=$PROFILE
os_id=$OS_ID
os_version_id=$OS_VERSION_ID
os_codename=$OS_CODENAME
deb_arch=$DEB_ARCH
machine_arch=$MACHINE_ARCH
python_abi=$PYTHON_ABI
node_version=$NODE_VERSION
project_commit=$PROJECT_COMMIT
apt_deb_count=$DEB_COUNT
python_wheel_count=$WHEEL_COUNT
EOF
cp "$DEPENDENCY_MANIFEST" "$BUNDLE_DIR/dependency_manifest.sh"
cp "$SCRIPT_DIR/install_offline.sh" "$BUNDLE_DIR/install_offline.sh"
chmod +x "$BUNDLE_DIR/install_offline.sh"
cat > "$BUNDLE_DIR/INSTALL.txt" <<EOF
RK3588 离线环境材料（$BUNDLE_NAME）

本目录由 offline_install_env_debian/install_offline.sh 自动调用，不需要手工进入。
把完整 offline_install_env_debian 目录放到对应版本项目根目录后执行：

   bash offline_install_env_debian/install_offline.sh

可选只读预检：
   bash offline_install_env_debian/install_offline.sh --verify-only

安装器会自动验收；以后复检：
   bash /userdata/rk3588_visual_analysis_framework/install_deps.sh --check
EOF

# apt-get 以 root 运行时会产生 root 所有的文件；改回制作用户后再封装。
as_root chown -R "$(id -u):$(id -g)" "$BUNDLE_DIR"
(
    cd "$BUNDLE_DIR"
    find . -type f ! -name SHA256SUMS -print0 \
        | LC_ALL=C sort -z | xargs -0 sha256sum > SHA256SUMS
    sha256sum -c --quiet SHA256SUMS
)

# 最后一步才替换正式目录：制包失败时，上一版 output/bundle 始终可用。
PREVIOUS_BUNDLE="$OUTPUT_DIR/.bundle.previous.$STAMP.$$"
if [ -e "$FINAL_BUNDLE_DIR" ]; then
    mv -- "$FINAL_BUNDLE_DIR" "$PREVIOUS_BUNDLE"
fi
if ! mv -- "$BUNDLE_DIR" "$FINAL_BUNDLE_DIR"; then
    if [ -e "$PREVIOUS_BUNDLE" ]; then
        mv -- "$PREVIOUS_BUNDLE" "$FINAL_BUNDLE_DIR"
    fi
    echo "[错误] 发布 output/bundle 失败；已尝试恢复上一版。" >&2
    exit 1
fi
if [ -e "$PREVIOUS_BUNDLE" ]; then
    rm -rf -- "$PREVIOUS_BUNDLE"
fi
BUILD_SUCCEEDED=true

BUNDLE_SIZE="$(du -sh "$FINAL_BUNDLE_DIR" | awk '{print $1}')"
echo
echo "[OK] 离线依赖材料制作完成："
echo "  $FINAL_BUNDLE_DIR  ($BUNDLE_SIZE)"
echo
echo "下一步：把整个 offline_install_env_debian 目录随同同版本项目复制到新设备，直接运行："
echo "  bash offline_install_env_debian/install_offline.sh"
