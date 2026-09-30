#!/bin/bash
# install_app.sh — 把根 build.sh 产出的程序包复制进 Web 控制台的 APPS_ROOT，
#                  让网页控制台识别到。重名时询问：覆盖(删旧存新) 或 改名。
#
# 用法: sudo ./install_app.sh <应用名>
#   例:  ./build.sh my_app && sudo ./install_app.sh my_app
#
# 交互式脚本，故不使用 set -e。

APPS_ROOT="${APPS_ROOT:-/opt/ai_apps}"
TARGET="vision_analysis"
SCRIPT_DIR="$(cd "$(dirname "$0")"; pwd)"
REPO_ROOT="$SCRIPT_DIR"

PKG="${1:-}"
if [ -z "$PKG" ]; then
    echo "用法: sudo $0 <应用名>   (对应 dist/<应用名>/)"
    exit 1
fi
case "$PKG" in
    */*|.|..) echo "[ERROR] 应用名只能是单层目录名: $PKG"; exit 1 ;;
esac

if [ "$(id -u)" -ne 0 ]; then
    echo "[ERROR] 需要写入 $APPS_ROOT，请用 sudo 运行。"
    exit 1
fi

# 程序包目录 = 仓库根 dist/<应用名>
PKG_DIR="$REPO_ROOT/dist/$PKG"
if [ ! -d "$PKG_DIR" ]; then
    echo "[ERROR] 程序包文件夹不存在: $PKG_DIR"
    echo "        先编译生成: cd $REPO_ROOT && ./build.sh $PKG"
    exit 1
fi
if [ ! -f "$PKG_DIR/$TARGET" ]; then
    echo "[ERROR] 程序包里找不到 $TARGET: $PKG_DIR/$TARGET"
    exit 1
fi

mkdir -p "$APPS_ROOT"

# 目标名默认 = 程序包名；重名则询问 覆盖 / 改名
DEST="$PKG"
while [ -e "$APPS_ROOT/$DEST" ]; do
    read -p "$APPS_ROOT/$DEST 已存在，覆盖它吗？(覆盖会先删除旧的) [y/N] " ans
    if [[ "$ans" =~ ^[Yy]$ ]]; then
        rm -rf "$APPS_ROOT/$DEST" || { echo "[ERROR] 删除旧目录失败"; exit 1; }
        break
    fi
    read -p "不覆盖。请输入一个新的名字: " DEST
    while [ -z "$DEST" ]; do
        read -p "名字不能为空，请重新输入: " DEST
    done
done

# 复制整个程序包到控制台目录
cp -r "$PKG_DIR" "$APPS_ROOT/$DEST" || { echo "[ERROR] 复制失败"; exit 1; }

echo ""
echo "✓ 已安装: $APPS_ROOT/$DEST"
echo "  打开 Web 控制台 → 程序管理 → 找到 [$DEST] → 点 [▶ 启动]"
