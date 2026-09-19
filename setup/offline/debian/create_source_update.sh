#!/usr/bin/env bash
# 为已经安装过 full-bundle 的设备生成轻量源码更新包。
#
# 用法（在项目根目录执行）：
#
#   bash setup/offline/debian/create_source_update.sh
#
# 也可以通过统一制包入口执行，效果完全相同：
#
#   bash setup/offline/debian/create_bundle.sh --source-only
#
# 适用条件：
#
#   - 目标设备已经成功安装过一次 full-bundle；
#   - 本次只修改 C/C++、Python、前端源码、配置或文档；
#   - 没有新增或修改系统、Python、npm、RKNN 等环境依赖。
#
# 本脚本不会编译 C/C++、不会构建前端、不会刷新 APT，也不会下载依赖。
# 它会对照最近一次 output/full-bundle 自动检查依赖边界；如果 requirements、
# package.json、package-lock.json、APT 依赖清单或固定 RKNN 文件发生变化，
# 会拒绝生成并提示改用 create_bundle.sh --full。
#
# 输出目录：
#
#   setup/offline/debian/output/source-update
#
# 把整个 source-update 目录复制到已经安装过完整包的目标设备，然后执行：
#
#   cd /userdata/source-update
#   sudo bash install_source_update.sh
#
# 全新设备不能只安装源码更新包，必须先生成并安装完整包：
#
#   bash setup/offline/debian/create_bundle.sh --full
#
# 查看帮助：
#
#   bash setup/offline/debian/create_source_update.sh --help
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
OUTPUT_DIR="$SCRIPT_DIR/output"
FULL_BUNDLE_DIR="$OUTPUT_DIR/full-bundle"
FINAL_UPDATE_DIR="$OUTPUT_DIR/source-update"
PACKAGE_NAME="vision-analysis-source"
BUILD_META_PACKAGE="vision-analysis-build-deps"
DEB_ARCH="arm64"
UPDATE_FORMAT=1

usage() {
    cat <<'EOF'
用法：bash setup/offline/debian/create_source_update.sh

快速生成只包含当前项目源码的更新包，不编译 C/C++、不构建前端、
不刷新 APT，也不重新下载系统依赖。

前提：至少成功生成过一次 output/full-bundle。目标设备也必须已经安装
该完整包；全新设备仍应使用 full-bundle/install_offline.sh。
EOF
}

case "${1:-}" in
    -h|--help)
        usage
        exit 0
        ;;
    "") ;;
    *)
        echo "[错误] 未知参数: $1" >&2
        usage >&2
        exit 2
        ;;
esac

required_commands=(awk cmp diff dpkg-deb du find git gzip mktemp readlink rsync sed sha256sum sort tar)
for command_name in "${required_commands[@]}"; do
    command -v "$command_name" >/dev/null 2>&1 \
        || { echo "[错误] 制作机缺少命令: $command_name" >&2; exit 1; }
done

[ -f "$FULL_BUNDLE_DIR/BUNDLE_INFO" ] \
    || { echo "[错误] 没有找到 $FULL_BUNDLE_DIR/BUNDLE_INFO。" >&2; \
         echo "       首次请先运行 create_bundle.sh --full。" >&2; exit 1; }

metadata_value() {
    local key="$1"
    sed -n "s/^${key}=//p" "$FULL_BUNDLE_DIR/BUNDLE_INFO" | head -n 1
}

BASE_PROFILE="$(metadata_value profile)"
BASE_PACKAGE_NAME="$(metadata_value source_package)"
BASE_PACKAGE_VERSION="$(metadata_value package_version)"
BASE_BUILD_META="$(metadata_value build_meta_package)"
if [ "$BASE_PROFILE" != runtime-build ] \
        || [ "$BASE_PACKAGE_NAME" != "$PACKAGE_NAME" ] \
        || [ -z "$BASE_PACKAGE_VERSION" ] \
        || [ "$BASE_BUILD_META" != "$BUILD_META_PACKAGE" ]; then
    echo "[错误] 现有 full-bundle 不是可用于源码更新的完整开发包。" >&2
    echo "       请先运行 create_bundle.sh --full 生成新的完整包。" >&2
    exit 1
fi

BASE_SOURCE_DEB="$FULL_BUNDLE_DIR/apt/${PACKAGE_NAME}_${BASE_PACKAGE_VERSION}_${DEB_ARCH}.deb"
[ -f "$BASE_SOURCE_DEB" ] \
    || { echo "[错误] 完整包缺少基准源码 deb: $BASE_SOURCE_DEB" >&2; exit 1; }

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
DEB_VERSION="2.0.${STAMP//[TZ]/}+src"
SOURCE_INSTALL_PATH="/userdata/rk3588_visual_analysis_framework-$DEB_VERSION"
STAGE_DIR="$(mktemp -d "$OUTPUT_DIR/.source-update.XXXXXX")"
BUILD_SUCCEEDED=false

cleanup() {
    [ ! -d "$STAGE_DIR" ] || rm -rf -- "$STAGE_DIR"
    if [ "$BUILD_SUCCEEDED" != true ]; then
        echo "[提示] 源码更新包生成失败，原 output/source-update 未改变。" >&2
    fi
}
trap cleanup EXIT

CURRENT_SOURCE="$STAGE_DIR/current-source"
BASE_PACKAGE_ROOT="$STAGE_DIR/base-package"
BASE_SOURCE="$STAGE_DIR/base-source"
PACKAGE_ROOT="$STAGE_DIR/package"
PACKAGE_SHARE="$PACKAGE_ROOT/usr/share/vision-analysis/source"
UPDATE_DIR="$STAGE_DIR/source-update"
mkdir -p "$CURRENT_SOURCE" "$BASE_PACKAGE_ROOT" "$BASE_SOURCE" \
    "$PACKAGE_ROOT/DEBIAN" "$PACKAGE_SHARE" "$UPDATE_DIR/apt"

copy_project_source() {
    local destination="$1"
    if git -C "$PROJECT_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        (
            cd "$PROJECT_ROOT"
            while IFS= read -r -d '' source_path; do
                [ "$source_path" = 'tools/first_net_config/first_net_config' ] && continue
                [ -e "$source_path" ] && printf '%s\0' "$source_path"
            done < <(git ls-files -z --cached --others --exclude-standard)
            # 工作区可能删除了 Git 已跟踪文件；不要让最后一个缺失路径的
            # [ -e ] 状态在 pipefail 下中止整个 rsync 管道。
            true
        ) | rsync -a --from0 --files-from=- "$PROJECT_ROOT/" "$destination/"
    else
        rsync -a \
            --exclude='.git/' --exclude='.claude/' --exclude='.vscode/' \
            --exclude='.pytest_cache/' --exclude='__pycache__/' --exclude='*.pyc' \
            --exclude='build/' --exclude='dist/' --exclude='node_modules/' \
            --exclude='setup/offline/*/output/' \
            --exclude='tools/first_net_config/first_net_config' \
            --exclude='*.mp4' --exclude='*.avi' --exclude='*.mkv' --exclude='*.docx' \
            "$PROJECT_ROOT/" "$destination/"
    fi
}

dependency_fingerprint() {
    local root="$1"
    local output="$2"
    (
        cd "$root"
        {
            for fixed_path in \
                setup/offline/debian/dependency_manifest.sh \
                setup/offline/debian/extra-runtime-packages.txt \
                setup/offline/debian/extra-build-packages.txt \
                web_console/frontend/package.json \
                web_console/frontend/package-lock.json; do
                [ ! -f "$fixed_path" ] || printf '%s\n' "$fixed_path"
            done
            find . -type f \
                \( -name 'requirements.txt' -o -name 'requirements-*.txt' \) \
                -not -path '*/node_modules/*' \
                -not -path '*/.offline-deb-app-*/*' -printf '%P\n'
            if [ -d vision_analysis/vendor/rknn ]; then
                find vision_analysis/vendor/rknn -type f -printf '%p\n'
            fi
        } | LC_ALL=C sort -u | while IFS= read -r source_path; do
            sha256sum "$source_path"
        done
    ) > "$output"
}

build_descriptor_fingerprint() {
    local root="$1"
    local output="$2"
    (
        cd "$root"
        find . -type f \
            \( -name 'CMakeLists.txt' -o -name '*.cmake' -o -name 'build.sh' \) \
            -not -path '*/node_modules/*' \
            -not -path '*/.offline-deb-app-*/*' -printf '%P\n' \
            | LC_ALL=C sort -u \
            | while IFS= read -r source_path; do sha256sum "$source_path"; done
    ) > "$output"
}

echo ">>> [1/4] 收集当前工作区源码..."
copy_project_source "$CURRENT_SOURCE"
[ -f "$CURRENT_SOURCE/vision_analysis/CMakeLists.txt" ] \
    || { echo "[错误] 当前源码缺少 vision_analysis/CMakeLists.txt。" >&2; exit 1; }
[ -f "$CURRENT_SOURCE/setup/install.sh" ] \
    || { echo "[错误] 当前源码缺少统一管理入口 install.sh。" >&2; exit 1; }

echo ">>> [2/4] 对照最近一次完整包检查依赖边界..."
dpkg-deb -x "$BASE_SOURCE_DEB" "$BASE_PACKAGE_ROOT"
BASE_ARCHIVE="$BASE_PACKAGE_ROOT/usr/share/vision-analysis/source/source.tar.gz"
[ -f "$BASE_ARCHIVE" ] \
    || { echo "[错误] 基准源码 deb 中没有 source.tar.gz。" >&2; exit 1; }
tar -xzf "$BASE_ARCHIVE" -C "$BASE_SOURCE"

dependency_fingerprint "$BASE_SOURCE" "$STAGE_DIR/base-dependencies.sha256"
dependency_fingerprint "$CURRENT_SOURCE" "$STAGE_DIR/current-dependencies.sha256"
if ! cmp -s "$STAGE_DIR/base-dependencies.sha256" "$STAGE_DIR/current-dependencies.sha256"; then
    echo "[错误] 检测到依赖清单、requirements、前端锁文件或 RKNN 固定文件发生变化。" >&2
    echo "       这些变化必须重新生成完整离线包：" >&2
    diff -u "$STAGE_DIR/base-dependencies.sha256" \
        "$STAGE_DIR/current-dependencies.sha256" >&2 || true
    echo "       请运行: bash setup/offline/debian/create_bundle.sh --full" >&2
    exit 1
fi

build_descriptor_fingerprint "$BASE_SOURCE" "$STAGE_DIR/base-build-files.sha256"
build_descriptor_fingerprint "$CURRENT_SOURCE" "$STAGE_DIR/current-build-files.sha256"
if ! cmp -s "$STAGE_DIR/base-build-files.sha256" "$STAGE_DIR/current-build-files.sha256"; then
    echo "[提示] CMake 或 build.sh 有变化；源码更新包可以生成。"
    echo "       如果这些修改新增了外部系统库，请改做 --full 完整包。"
fi

echo ">>> [3/4] 封装源码 deb（不携带重复的 node_modules）..."
tar -C "$CURRENT_SOURCE" -czf "$PACKAGE_SHARE/source.tar.gz" .
cat > "$PACKAGE_SHARE/README.txt" <<EOF
这是基于完整环境 $BASE_PACKAGE_VERSION 生成的快速源码更新 $DEB_VERSION。
实际目录: $SOURCE_INSTALL_PATH
固定入口: /userdata/rk3588_visual_analysis_framework

本包不重复携带 node_modules；安装时会复用上一版完整源码中的前端依赖。

主程序编译与安装：
  cd $SOURCE_INSTALL_PATH
  ./vision package projects/person_count
  sudo ./install_app.sh dist

首次网络配置工具编译：
  cd $SOURCE_INSTALL_PATH/tools/first_net_config
  ./build.sh
EOF

cat > "$PACKAGE_ROOT/DEBIAN/control" <<EOF
Package: $PACKAGE_NAME
Version: $DEB_VERSION
Architecture: $DEB_ARCH
Depends: tar, $BUILD_META_PACKAGE (>= $BASE_PACKAGE_VERSION)
Maintainer: Vision Analysis Project
Description: Fast source-only update for RK3588 vision analysis
EOF

cat > "$PACKAGE_ROOT/DEBIAN/postinst" <<EOF
#!/bin/sh
set -e
version_dir='$SOURCE_INSTALL_PATH'
stable_path='/userdata/rk3588_visual_analysis_framework'
archive='/usr/share/vision-analysis/source/source.tar.gz'

previous_path=''
if [ -L "\$stable_path" ]; then
    previous_path="\$(readlink -f "\$stable_path" 2>/dev/null || true)"
elif [ -d "\$stable_path" ]; then
    previous_path="\$stable_path"
fi

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
if [ ! -e "\$frontend_dir/node_modules" ] \
        && [ -n "\$previous_path" ] \
        && [ -d "\$previous_path/web_console/frontend/node_modules" ]; then
    previous_modules="\$(readlink -f \
        "\$previous_path/web_console/frontend/node_modules" 2>/dev/null || true)"
    [ -z "\$previous_modules" ] \
        || ln -s "\$previous_modules" "\$frontend_dir/node_modules"
fi

if [ -L "\$stable_path" ] || [ ! -e "\$stable_path" ]; then
    ln -sfn "\$version_dir" "\$stable_path"
else
    echo "[提示] \$stable_path 是现有真实目录，未覆盖它。" >&2
    echo "       新版源码位于: \$version_dir" >&2
fi

if [ ! -e "\$frontend_dir/node_modules" ]; then
    echo "[警告] 没有找到可复用的前端 node_modules；C/C++ 源码仍可正常编译。" >&2
    echo "       如需构建前端，请重新安装完整 full-bundle。" >&2
fi
EOF

cat > "$PACKAGE_ROOT/DEBIAN/prerm" <<EOF
#!/bin/sh
set -e
version_dir='$SOURCE_INSTALL_PATH'
stable_path='/userdata/rk3588_visual_analysis_framework'
if [ "\${1:-}" = remove ] && [ -L "\$stable_path" ] \
        && [ "\$(readlink "\$stable_path")" = "\$version_dir" ]; then
    rm -f "\$stable_path"
fi
EOF
chmod 0755 "$PACKAGE_ROOT/DEBIAN/postinst" "$PACKAGE_ROOT/DEBIAN/prerm"

SOURCE_DEB_NAME="${PACKAGE_NAME}_${DEB_VERSION}_${DEB_ARCH}.deb"
dpkg-deb --build --root-owner-group "$PACKAGE_ROOT" \
    "$UPDATE_DIR/apt/$SOURCE_DEB_NAME" >/dev/null

cat > "$UPDATE_DIR/UPDATE_INFO" <<EOF
update_format=$UPDATE_FORMAT
created_at=$STAMP
deb_arch=$DEB_ARCH
package_name=$PACKAGE_NAME
package_version=$DEB_VERSION
source_install_path=$SOURCE_INSTALL_PATH
required_build_meta=$BUILD_META_PACKAGE
required_build_version=$BASE_PACKAGE_VERSION
EOF
cp "$SCRIPT_DIR/templates/install_source_update.sh" \
    "$UPDATE_DIR/install_source_update.sh"
chmod 0755 "$UPDATE_DIR/install_source_update.sh"
(
    cd "$UPDATE_DIR"
    sha256sum "apt/$SOURCE_DEB_NAME" > SHA256SUMS
)

echo ">>> [4/4] 原子发布源码更新包..."
PREVIOUS="$OUTPUT_DIR/.source-update.previous.$$"
if [ -e "$FINAL_UPDATE_DIR" ]; then
    mv -- "$FINAL_UPDATE_DIR" "$PREVIOUS"
fi
if ! mv -- "$UPDATE_DIR" "$FINAL_UPDATE_DIR"; then
    [ ! -e "$PREVIOUS" ] || mv -- "$PREVIOUS" "$FINAL_UPDATE_DIR"
    echo "[错误] 发布失败，已恢复上一版源码更新包。" >&2
    exit 1
fi
rm -rf -- "$PREVIOUS"

BUILD_SUCCEEDED=true
UPDATE_SIZE="$(du -sh "$FINAL_UPDATE_DIR" | awk '{print $1}')"
echo
echo "[OK] 源码更新包已生成：$FINAL_UPDATE_DIR"
echo "     大小: $UPDATE_SIZE"
echo "     基于完整环境: $BASE_PACKAGE_VERSION"
echo "     未执行 C/C++ 编译、前端构建、APT 刷新或依赖下载。"
echo "     复制 source-update 到已装过完整包的设备后执行："
echo "       sudo bash install_source_update.sh"
