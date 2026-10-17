#!/bin/sh
# 为源码更新复用本机前端依赖；缺少目录时从 npm 本地缓存恢复，不联网下载。
set -eu

project_dir=${1:?用法：sh restore_frontend_dependencies.sh 新版源码目录}
frontend_dir="$project_dir/web_console/frontend"
lock_file="$frontend_dir/package-lock.json"
source_parent=$(dirname "$project_dir")
cache_root=${VISION_FRONTEND_CACHE_ROOT:-/var/lib/vision-analysis/frontend-dependencies}

[ -f "$lock_file" ] || { echo "[错误] 缺少前端锁文件：$lock_file" >&2; exit 1; }
lock_hash=$(sha256sum "$lock_file")
lock_hash=${lock_hash%% *}
cache_dir="$cache_root/$lock_hash"

dependencies_match() {
    [ -f "$1/package-lock.json" ] \
        && cmp -s "$lock_file" "$1/package-lock.json" \
        && [ -x "$1/node_modules/.bin/tsc" ] \
        && [ -x "$1/node_modules/.bin/vite" ]
}

if ! dependencies_match "$cache_dir"; then
    donor_dir=''
    for candidate in "$frontend_dir" \
            "$source_parent/rk3588_visual_analysis_framework/web_console/frontend" \
            "$source_parent"/rk3588_visual_analysis_framework-*/web_console/frontend; do
        if dependencies_match "$candidate"; then
            donor_dir=$candidate
            break
        fi
    done
    # 缓存使用独立副本，旧源码目录被清理后依赖仍可继续使用。
    mkdir -p "$cache_root"
    stage=$(mktemp -d "$cache_root/.restore.XXXXXX")
    trap 'rm -rf -- "$stage"' EXIT HUP INT TERM
    cp "$lock_file" "$stage/package-lock.json"
    if [ -n "$donor_dir" ]; then
        mkdir "$stage/node_modules"
        donor_modules=$(readlink -f "$donor_dir/node_modules")
        cp -a "$donor_modules/." "$stage/node_modules/"
    else
        if ! command -v node >/dev/null 2>&1 || ! command -v npm >/dev/null 2>&1; then
            echo "[错误] 未找到匹配的前端依赖，且缺少 Node.js/npm，无法从本地缓存恢复。" >&2
            exit 1
        fi
        [ -f "$frontend_dir/package.json" ] \
            || { echo "[错误] 缺少前端 package.json。" >&2; exit 1; }
        cp "$frontend_dir/package.json" "$stage/package.json"
        echo "[准备] 没有匹配的 node_modules，尝试从 npm 本地缓存离线恢复..."
        if ! (cd "$stage" && npm_config_offline=true npm ci --offline --no-audit --no-fund); then
            echo "[错误] 本地 npm 缓存不足，无法恢复当前 package-lock.json 的前端依赖。" >&2
            echo "       未联网下载。请从配套完整离线包恢复，或联网时执行 ./install.sh online 准备依赖。" >&2
            exit 1
        fi
        donor_dir='npm 本地缓存（未联网）'
    fi
    dependencies_match "$stage" \
        || { echo "[错误] 本地前端依赖复制不完整。" >&2; exit 1; }
    if [ -e "$cache_dir" ] || [ -L "$cache_dir" ]; then
        backup_dir=$(mktemp -d "$cache_root/.incomplete.XXXXXX")
        mv "$cache_dir" "$backup_dir/dependencies"
    fi
    mv "$stage" "$cache_dir"
    trap - EXIT HUP INT TERM
    echo "[OK] 已从 $donor_dir 保存本地前端依赖到独立缓存。"
fi

mkdir -p "$frontend_dir"
if [ -L "$frontend_dir/node_modules" ]; then
    # 包括失效链接和指向移动入口的自循环链接。
    rm "$frontend_dir/node_modules"
elif [ -e "$frontend_dir/node_modules" ]; then
    # 完整的真实依赖目录不改动；不完整目录也不直接删除。
    if dependencies_match "$frontend_dir"; then
        exit 0
    fi
    backup_dir=$(mktemp -d "$frontend_dir/.incomplete-dependencies.XXXXXX")
    mv "$frontend_dir/node_modules" "$backup_dir/node_modules"
fi
ln -s "$cache_dir/node_modules" "$frontend_dir/node_modules"
echo "[OK] 前端依赖已就绪：$frontend_dir/node_modules -> $cache_dir/node_modules"
