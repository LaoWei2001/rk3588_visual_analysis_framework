#!/usr/bin/env bash
# 从 create_bundle.sh 生成的本地材料安装项目运行/编译环境，全程不访问公网。
set -Eeuo pipefail

BUNDLE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 项目里的顶层脚本是统一入口；真正的材料固定放在 output/bundle。
# 制包时同一份脚本会被复制进 bundle，那里存在 BUNDLE_INFO，因此不会再次转发。
if [ ! -f "$BUNDLE_DIR/BUNDLE_INFO" ] || [ ! -f "$BUNDLE_DIR/SHA256SUMS" ]; then
    PAYLOAD_DIR="$BUNDLE_DIR/output/bundle"
    if [ -f "$PAYLOAD_DIR/BUNDLE_INFO" ] && [ -f "$PAYLOAD_DIR/SHA256SUMS" ] \
            && [ -f "$PAYLOAD_DIR/install_offline.sh" ]; then
        echo ">>> 使用离线材料: $PAYLOAD_DIR"
        exec bash "$PAYLOAD_DIR/install_offline.sh" "$@"
    fi
    if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
        cat <<'EOF'
用法：bash offline_install_env_debian/install_offline.sh [--verify-only] [--build]

把完整 offline_install_env_debian 目录放在项目根目录。正式安装无需其他参数；
安装器会自动使用 output/bundle 并识别项目根目录。
EOF
        exit 0
    fi
    echo "[错误] 缺少完整离线材料: $PAYLOAD_DIR" >&2
    echo "       请在有公网的同型号开发机运行：" >&2
    echo "         bash offline_install_env_debian/create_bundle.sh" >&2
    echo "       然后把整个 offline_install_env_debian 目录复制到本项目的相同位置。" >&2
    exit 1
fi

PROJECT_ROOT=""
WANT_BUILD=false
VERIFY_ONLY=false
APT_WORK_DIR=""
FRONTEND_STAGE=""

usage() {
    cat <<'EOF'
用法：bash offline_install_env_debian/install_offline.sh [选项]

选项：
  --project-root <目录>  高级选项：指定项目根目录（通常无需使用）
  --build                同时安装并验收 C/C++ 编译环境
  --verify-only          只校验包、平台和项目版本，不安装任何内容
  -h, --help             显示帮助

脚本只读取离线包内的 deb、wheel、Node 和前端归档，不使用公网软件源。
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --project-root)
            [ "$#" -ge 2 ] || { echo "[错误] --project-root 缺少目录" >&2; exit 2; }
            PROJECT_ROOT="$2"
            shift
            ;;
        --build) WANT_BUILD=true ;;
        --verify-only) VERIFY_ONLY=true ;;
        -h|--help) usage; exit 0 ;;
        *) echo "[错误] 未知参数: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

cleanup() {
    if [ -n "$APT_WORK_DIR" ] && [ -d "$APT_WORK_DIR" ]; then
        if declare -F as_root >/dev/null 2>&1; then
            as_root rm -rf -- "$APT_WORK_DIR"
        else
            rm -rf -- "$APT_WORK_DIR"
        fi
    fi
    if [ -n "$FRONTEND_STAGE" ] && [ -d "$FRONTEND_STAGE" ]; then
        if declare -F as_root >/dev/null 2>&1; then
            as_root rm -rf -- "$FRONTEND_STAGE"
        else
            rm -rf -- "$FRONTEND_STAGE"
        fi
    fi
}
trap cleanup EXIT

for command_name in apt-get awk comm dpkg dpkg-query grep id mktemp sed sha256sum sort stat tar uname; do
    command -v "$command_name" >/dev/null 2>&1 \
        || { echo "[错误] 系统缺少离线安装所需的基础命令: $command_name" >&2; exit 1; }
done

echo ">>> [preflight] 校验离线包完整性..."
(
    cd "$BUNDLE_DIR"
    sha256sum -c --quiet SHA256SUMS
) || { echo "[错误] 离线包文件损坏或不完整，停止安装。" >&2; exit 1; }

# sha256sum 会检查清单内的文件，但默认忽略目录里额外残留的旧文件。显式拒绝多余文件，
# 避免用户把新 bundle 合并复制到旧目录后，pip 意外选中旧 wheel。
EXTRA_BUNDLE_FILES="$(
    cd "$BUNDLE_DIR"
    comm -13 \
        <(sed 's/^[^ ]*  //' SHA256SUMS | LC_ALL=C sort) \
        <(find . -type f ! -name SHA256SUMS -print | LC_ALL=C sort)
)"
if [ -n "$EXTRA_BUNDLE_FILES" ]; then
    echo "[错误] 离线材料目录混入了清单之外的旧文件：" >&2
    printf '%s\n' "$EXTRA_BUNDLE_FILES" | sed -n '1,12s/^/       /p' >&2
    echo "       请完整替换 offline_install_env_debian/output/bundle，不要与旧目录合并。" >&2
    exit 1
fi

metadata_value() {
    local key="$1"
    awk -F= -v wanted="$key" '$1 == wanted {sub(/^[^=]*=/, ""); print; exit}' \
        "$BUNDLE_DIR/BUNDLE_INFO"
}

EXPECTED_FORMAT="$(metadata_value bundle_format)"
EXPECTED_PROFILE="$(metadata_value profile)"
EXPECTED_OS_ID="$(metadata_value os_id)"
EXPECTED_OS_VERSION="$(metadata_value os_version_id)"
EXPECTED_DEB_ARCH="$(metadata_value deb_arch)"
EXPECTED_MACHINE_ARCH="$(metadata_value machine_arch)"
EXPECTED_PYTHON_ABI="$(metadata_value python_abi)"
NODE_VERSION="$(metadata_value node_version)"

for metadata_item in "$EXPECTED_FORMAT" "$EXPECTED_PROFILE" "$EXPECTED_OS_ID" \
        "$EXPECTED_OS_VERSION" "$EXPECTED_DEB_ARCH" "$EXPECTED_MACHINE_ARCH" \
        "$EXPECTED_PYTHON_ABI" "$NODE_VERSION"; do
    [[ "$metadata_item" =~ ^[A-Za-z0-9._+-]+$ ]] \
        || { echo "[错误] BUNDLE_INFO 字段无效: $metadata_item" >&2; exit 1; }
done
[ "$EXPECTED_FORMAT" = "1" ] \
    || { echo "[错误] 不支持的离线包格式: $EXPECTED_FORMAT" >&2; exit 1; }
case "$EXPECTED_PROFILE" in
    runtime|runtime-build) ;;
    *) echo "[错误] 不支持的离线包类型: $EXPECTED_PROFILE" >&2; exit 1 ;;
esac

# shellcheck source=/etc/os-release
source /etc/os-release
CURRENT_OS_ID="${ID:-unknown}"
CURRENT_OS_VERSION="${VERSION_ID:-unknown}"
CURRENT_DEB_ARCH="$(dpkg --print-architecture)"
CURRENT_MACHINE_ARCH="$(uname -m)"
CURRENT_PYTHON_ABI="missing"
if command -v python3 >/dev/null 2>&1; then
    CURRENT_PYTHON_ABI="$(python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')"
fi

if [ "$CURRENT_OS_ID" != "$EXPECTED_OS_ID" ] \
    || [ "$CURRENT_OS_VERSION" != "$EXPECTED_OS_VERSION" ] \
    || [ "$CURRENT_DEB_ARCH" != "$EXPECTED_DEB_ARCH" ] \
    || [ "$CURRENT_MACHINE_ARCH" != "$EXPECTED_MACHINE_ARCH" ]; then
    echo "[错误] 离线包与现场机平台不一致，拒绝安装：" >&2
    echo "       离线包: $EXPECTED_OS_ID $EXPECTED_OS_VERSION, $EXPECTED_MACHINE_ARCH/$EXPECTED_DEB_ARCH, Python $EXPECTED_PYTHON_ABI" >&2
    echo "       现场机: $CURRENT_OS_ID $CURRENT_OS_VERSION, $CURRENT_MACHINE_ARCH/$CURRENT_DEB_ARCH, Python $CURRENT_PYTHON_ABI" >&2
    exit 1
fi
if [ "$CURRENT_PYTHON_ABI" != "missing" ] \
    && [ "$CURRENT_PYTHON_ABI" != "$EXPECTED_PYTHON_ABI" ]; then
    echo "[错误] 现场 Python $CURRENT_PYTHON_ABI 与离线 wheel 的 Python $EXPECTED_PYTHON_ABI 不一致。" >&2
    exit 1
fi
if [ "$WANT_BUILD" = true ] && [ "$EXPECTED_PROFILE" != "runtime-build" ]; then
    echo "[错误] 当前包只含运行环境；请用 create_bundle.sh --build 重新制作。" >&2
    exit 1
fi
if [ "$EXPECTED_PROFILE" = "runtime-build" ]; then
    WANT_BUILD=true
fi

if [ -n "$PROJECT_ROOT" ]; then
    [ -d "$PROJECT_ROOT" ] \
        || { echo "[错误] --project-root 指定的目录不存在: $PROJECT_ROOT" >&2; exit 1; }
    PROJECT_ROOT="$(cd "$PROJECT_ROOT" && pwd)"
    if [ ! -f "$PROJECT_ROOT/install_deps.sh" ]; then
        echo "[错误] --project-root 不是项目根目录: $PROJECT_ROOT" >&2
        if [ -f "$PROJECT_ROOT/BUNDLE_INFO" ]; then
            echo "       当前路径是离线材料目录；通常完全不需要传 --project-root。" >&2
        else
            echo "       项目根目录中必须存在 install_deps.sh。" >&2
        fi
        exit 1
    fi
else
    # 优先使用 bundle 所属项目，保证从任意当前目录执行绝对路径时也不会误选其他项目。
    project_candidate="$BUNDLE_DIR"
    for _ in 1 2 3 4; do
        project_candidate="$(dirname "$project_candidate")"
        if [ -f "$project_candidate/install_deps.sh" ]; then
            PROJECT_ROOT="$project_candidate"
            break
        fi
    done
    if [ -z "$PROJECT_ROOT" ] && [ -f "$PWD/install_deps.sh" ]; then
        PROJECT_ROOT="$PWD"
    fi
    if [ -z "$PROJECT_ROOT" ]; then
        echo "[错误] 无法自动找到项目根目录。" >&2
        echo "       offline_install_env_debian 必须位于项目根目录；项目中应存在 install_deps.sh。" >&2
        exit 1
    fi
    PROJECT_ROOT="$(cd "$PROJECT_ROOT" && pwd)"
    echo "[提示] 已自动识别项目根目录: $PROJECT_ROOT"
fi
FRONTEND_DIR="$PROJECT_ROOT/web_console/frontend"
[ -d "$FRONTEND_DIR" ] \
    || { echo "[错误] 缺少前端目录: $FRONTEND_DIR" >&2; exit 1; }

echo "       离线包: $BUNDLE_DIR"
echo "       项目根: $PROJECT_ROOT"
echo "       包类型: $EXPECTED_PROFILE"

echo ">>> [preflight] 核对离线包与项目版本..."
verify_project_manifest() {
    local root_dir="$1"
    local manifest="$2"
    local label="$3"
    local check_output
    if check_output="$(cd "$root_dir" && sha256sum -c --quiet "$manifest" 2>&1)"; then
        return 0
    fi
    echo "[错误] $label 与制包时不一致：" >&2
    printf '%s\n' "$check_output" | sed 's/^/       /' >&2
    echo "       请复制制包时使用的同版本项目，或基于当前项目重新制包。" >&2
    echo "       不要修改 SHA256SUMS 绕过检查。" >&2
    return 1
}

verify_project_manifest \
    "$PROJECT_ROOT" \
    "$BUNDLE_DIR/project/REQUIREMENTS_SHA256SUMS" \
    "依赖清单/Python requirements" \
    || exit 1
verify_project_manifest \
    "$FRONTEND_DIR" \
    "$BUNDLE_DIR/project/FRONTEND_SHA256SUMS" \
    "前端构建输入/package-lock.json" \
    || exit 1

if [ "$VERIFY_ONLY" = true ]; then
    if [ "$CURRENT_PYTHON_ABI" = "missing" ]; then
        echo "[提示] 当前未安装 python3；正式安装时将先从包内 APT 仓库安装。"
    fi
    echo "[OK] 离线包完整性、平台和项目版本校验全部通过；未安装或修改任何内容。"
    echo "     下一步：重新运行本命令并删除 --verify-only，即可正式安装。"
    exit 0
fi

if [ "$(id -u)" -eq 0 ]; then
    ROOT=()
elif command -v sudo >/dev/null 2>&1; then
    ROOT=(sudo)
else
    echo "[错误] 安装系统环境需要 root 或 sudo。" >&2
    exit 1
fi
as_root() {
    "${ROOT[@]}" "$@"
}

apt_package_is_installed() {
    dpkg-query -W -f='${db:Status-Status}\n' "$1" 2>/dev/null | grep -qx installed
}

install_local_apt_packages() {
    local group_name="$1"
    shift
    local missing=()
    local package
    for package in "$@"; do
        apt_package_is_installed "$package" || missing+=("$package")
    done
    if [ "${#missing[@]}" -eq 0 ]; then
        echo "    ${group_name}已全部安装；保留当前版本。"
        return
    fi
    echo "    ${group_name}缺失 ${#missing[@]} 个包：${missing[*]}"
    as_root env DEBIAN_FRONTEND=noninteractive apt-get \
        -o APT::Sandbox::User=root \
        -o "Dir::Etc::sourcelist=$APT_WORK_DIR/sources.list" \
        -o "Dir::Etc::sourceparts=$APT_WORK_DIR/sourceparts" \
        -o "Dir::State::lists=$APT_WORK_DIR/lists" \
        -o "Dir::Cache::archives=$APT_WORK_DIR/cache/archives" \
        -o Acquire::Languages=none \
        -o Acquire::Retries=0 \
        --no-install-recommends --no-upgrade -y install "${missing[@]}"
}

echo ">>> [1/5] 从包内 APT 仓库安装系统依赖..."
if [[ "$BUNDLE_DIR" =~ [[:space:]] ]]; then
    echo "[错误] 离线包路径不能包含空格: $BUNDLE_DIR" >&2
    exit 1
fi
APT_WORK_DIR="$(mktemp -d)"
mkdir -p "$APT_WORK_DIR/lists/partial" "$APT_WORK_DIR/cache/archives/partial" \
    "$APT_WORK_DIR/sourceparts"
printf 'deb [trusted=yes] file:%s/apt ./\n' "$BUNDLE_DIR" > "$APT_WORK_DIR/sources.list"
# 临时目录默认是 0700；材料已经通过 SHA-256 校验，这里让仅访问本地 file: 仓库的
# APT 使用 root，避免 Debian 因 _apt 无法穿越临时目录而输出误导性的降权警告。
as_root apt-get \
    -o APT::Sandbox::User=root \
    -o "Dir::Etc::sourcelist=$APT_WORK_DIR/sources.list" \
    -o "Dir::Etc::sourceparts=$APT_WORK_DIR/sourceparts" \
    -o "Dir::State::lists=$APT_WORK_DIR/lists" \
    -o "Dir::Cache::archives=$APT_WORK_DIR/cache/archives" \
    -o Acquire::Languages=none \
    -o Acquire::IndexTargets::deb::Contents-deb::DefaultEnabled=false \
    -o Acquire::Retries=0 update
mapfile -t APT_RUNTIME_PACKAGES < "$BUNDLE_DIR/apt/runtime-packages.txt"
install_local_apt_packages "运行环境" "${APT_RUNTIME_PACKAGES[@]}"
if [ "$WANT_BUILD" = true ]; then
    mapfile -t APT_BUILD_PACKAGES < "$BUNDLE_DIR/apt/build-packages.txt"
    install_local_apt_packages "C/C++ 编译环境" "${APT_BUILD_PACKAGES[@]}"
fi
command -v python3 >/dev/null 2>&1 \
    || { echo "[错误] APT 阶段完成后仍未找到 python3" >&2; exit 1; }
CURRENT_PYTHON_ABI="$(python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')"
[ "$CURRENT_PYTHON_ABI" = "$EXPECTED_PYTHON_ABI" ] \
    || { echo "[错误] 安装后的 Python $CURRENT_PYTHON_ABI 与 wheelhouse $EXPECTED_PYTHON_ABI 不一致" >&2; exit 1; }

echo ">>> [2/5] 安装包内 Node.js（系统已有 18+ 时保留）..."
node_major_is_supported() {
    command -v node >/dev/null 2>&1 || return 1
    local major
    major="$(node -v 2>/dev/null | sed -n 's/^v\([0-9][0-9]*\).*/\1/p')"
    [ -n "$major" ] && [ "$major" -ge 18 ]
}
if node_major_is_supported && command -v npm >/dev/null 2>&1; then
    echo "    已有 Node $(node -v) / npm $(npm -v)，保留当前版本。"
else
    NODE_PACKAGE="node-${NODE_VERSION}-linux-arm64"
    NODE_ARCHIVE="$BUNDLE_DIR/node/$NODE_PACKAGE.tar.xz"
    [ -f "$NODE_ARCHIVE" ] || { echo "[错误] 缺少 $NODE_ARCHIVE" >&2; exit 1; }
    as_root mkdir -p /usr/local/lib/nodejs /usr/local/bin
    as_root tar -xJf "$NODE_ARCHIVE" -C /usr/local/lib/nodejs
    for executable in node npm npx corepack; do
        if [ -e "/usr/local/lib/nodejs/$NODE_PACKAGE/bin/$executable" ]; then
            as_root ln -sfn "/usr/local/lib/nodejs/$NODE_PACKAGE/bin/$executable" "/usr/local/bin/$executable"
        fi
    done
    hash -r 2>/dev/null || true
    node_major_is_supported && command -v npm >/dev/null 2>&1 \
        || { echo "[错误] Node.js 离线安装后仍不可用" >&2; exit 1; }
    echo "    已安装 Node $(node -v) / npm $(npm -v)。"
fi

echo ">>> [3/5] 从 wheelhouse 安装 Python 依赖..."
PIP_SYSTEM_ARGS=()
PIP_INSTALL_HELP="$(python3 -m pip help install 2>/dev/null || true)"
if grep -q -- '--break-system-packages' <<< "$PIP_INSTALL_HELP"; then
    PIP_SYSTEM_ARGS+=(--break-system-packages)
fi
as_root python3 -m pip install "${PIP_SYSTEM_ARGS[@]}" \
    --no-index --find-links "$BUNDLE_DIR/python/wheelhouse" \
    --upgrade pip setuptools wheel
PIP_REQUIREMENT_ARGS=()
while IFS= read -r requirement_name; do
    [ -n "$requirement_name" ] || continue
    PIP_REQUIREMENT_ARGS+=(-r "$BUNDLE_DIR/python/requirements/$requirement_name")
done < "$BUNDLE_DIR/python/requirements.list"
as_root python3 -m pip install "${PIP_SYSTEM_ARGS[@]}" \
    --no-index --find-links "$BUNDLE_DIR/python/wheelhouse" \
    --prefer-binary "${PIP_REQUIREMENT_ARGS[@]}"
python3 -m pip check

echo ">>> [4/5] 恢复锁定的前端依赖和预构建页面..."
FRONTEND_OWNER="$(stat -c '%u:%g' "$FRONTEND_DIR")"
FRONTEND_STAGE="$(as_root mktemp -d "$FRONTEND_DIR/.offline-install.XXXXXX")"
as_root tar --no-same-owner -xzf "$BUNDLE_DIR/frontend/node_modules.tar.gz" -C "$FRONTEND_STAGE"
as_root tar --no-same-owner -xzf "$BUNDLE_DIR/frontend/dist.tar.gz" -C "$FRONTEND_STAGE"
as_root chown -R "$(id -u):$(id -g)" "$FRONTEND_STAGE"
as_root chmod 0755 "$FRONTEND_STAGE"
[ -d "$FRONTEND_STAGE/node_modules" ] && [ -f "$FRONTEND_STAGE/dist/index.html" ] \
    || { echo "[错误] 前端离线归档内容不完整" >&2; exit 1; }
as_root cp "$BUNDLE_DIR/frontend/package.json" "$BUNDLE_DIR/frontend/package-lock.json" "$FRONTEND_STAGE/"
NPM_TREE_CHECK=""
if ! NPM_TREE_CHECK="$(cd "$FRONTEND_STAGE" && npm ls --depth=0 2>&1)"; then
    echo "[错误] 包内前端依赖树自身校验失败；尚未替换项目现有 node_modules。" >&2
    printf '%s\n' "$NPM_TREE_CHECK" | sed -n '1,20s/^/       /p' >&2
    exit 1
fi
as_root rm -f -- "$FRONTEND_STAGE/package.json" "$FRONTEND_STAGE/package-lock.json"

as_root rm -rf -- "$FRONTEND_DIR/node_modules" "$FRONTEND_DIR/dist"
as_root mv -- "$FRONTEND_STAGE/node_modules" "$FRONTEND_DIR/node_modules"
as_root mv -- "$FRONTEND_STAGE/dist" "$FRONTEND_DIR/dist"
as_root chown -R "$FRONTEND_OWNER" "$FRONTEND_DIR/node_modules" "$FRONTEND_DIR/dist"
echo "    前端 node_modules 与 package-lock.json 校验通过，预构建页面已恢复。"

echo ">>> [5/5] 调用项目只读检查验收环境..."
CHECK_ARGS=(--check)
[ "$WANT_BUILD" = true ] && CHECK_ARGS+=(--build)
bash "$PROJECT_ROOT/install_deps.sh" "${CHECK_ARGS[@]}"

echo
echo "[OK] 离线环境安装完成，全程未使用公网软件源。"
echo "如需安装/重装 Web 控制台服务："
if [ "$(id -u)" -eq 0 ]; then
    echo "  env OFFLINE=1 bash $PROJECT_ROOT/web_console/install.sh"
else
    echo "  sudo env OFFLINE=1 bash $PROJECT_ROOT/web_console/install.sh"
fi
