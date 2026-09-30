#!/usr/bin/env python3
"""
检查 Dify 测试服务器收到的文件，支持图片预览和视频信息提取。

用法:
    python3 decode_files.py                    # 列出所有收到的文件
    python3 decode_files.py --detail           # 详细信息（尺寸、时长等）
    python3 decode_files.py --open seq0001     # 用 ffplay/feh 打开指定文件

依赖（可选，没有也能跑基础功能）:
    pip install pillow    # 图片尺寸和格式验证
    ffprobe               # 视频信息（ffmpeg 自带）
"""

import argparse
import json
import os
import struct
import sys

FILES_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "received_files")
RECEIVED_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "received")


def _human_size(size_bytes: int) -> str:
    if size_bytes < 1024:
        return f"{size_bytes} B"
    elif size_bytes < 1024 * 1024:
        return f"{size_bytes / 1024:.1f} KB"
    else:
        return f"{size_bytes / (1024 * 1024):.2f} MB"


def _get_image_info(path: str) -> dict:
    """用 PIL 获取图片信息。"""
    try:
        from PIL import Image
        with Image.open(path) as img:
            return {
                "format": img.format,
                "size": f"{img.width}x{img.height}",
                "mode": img.mode,
            }
    except ImportError:
        return {}
    except Exception as e:
        return {"error": str(e)}


def _guess_image_info(path: str) -> dict:
    """不需要 PIL 的基础图片识别（通过文件头魔数）。"""
    try:
        with open(path, "rb") as f:
            header = f.read(24)
    except Exception:
        return {}

    if header[:2] == b"\xff\xd8":
        return {"format": "JPEG"}
    elif header[:8] == b"\x89PNG\r\n\x1a\n":
        return {"format": "PNG"}
    elif header[:4] == b"GIF8":
        return {"format": "GIF"}
    elif header[:2] in (b"BM",):
        return {"format": "BMP"}
    elif header[:4] == b"RIFF" and header[8:12] == b"WEBP":
        return {"format": "WEBP"}
    return {}


def _get_video_info(path: str) -> dict:
    """用 ffprobe 获取视频信息。"""
    import subprocess
    try:
        result = subprocess.run(
            ["ffprobe", "-v", "quiet", "-print_format", "json", "-show_format",
             "-show_streams", path],
            capture_output=True, text=True, timeout=10,
        )
        if result.returncode != 0:
            return {}
        data = json.loads(result.stdout)
        fmt = data.get("format", {})
        video_streams = [s for s in data.get("streams", []) if s.get("codec_type") == "video"]
        info = {
            "duration": fmt.get("duration", "?"),
            "size": _human_size(int(fmt.get("size", 0))),
            "bit_rate": fmt.get("bit_rate", "?"),
        }
        if video_streams:
            vs = video_streams[0]
            info.update({
                "codec": vs.get("codec_name", "?"),
                "resolution": f"{vs.get('width', '?')}x{vs.get('height', '?')}",
                "fps": vs.get("r_frame_rate", "?"),
            })
        return info
    except FileNotFoundError:
        return {"error": "ffprobe 未安装"}
    except Exception as e:
        return {"error": str(e)}


def _find_matching_record(filename: str) -> str | None:
    """根据文件名中的 seq 编号找到对应的请求记录。"""
    # filename like: seq0001_file_annotated.jpg
    if not filename.startswith("seq"):
        return None
    seq = filename.split("_")[0]  # seq0001
    if not os.path.isdir(RECEIVED_DIR):
        return None
    for name in sorted(os.listdir(RECEIVED_DIR)):
        if not name.endswith(".json"):
            continue
        path = os.path.join(RECEIVED_DIR, name)
        try:
            with open(path, "r", encoding="utf-8") as f:
                record = json.load(f)
            if isinstance(record, dict) and record.get("seq") is not None:
                if f"seq{record['seq']:04d}" == seq:
                    return name
        except Exception:
            pass
    return None


def list_files(detail: bool = False):
    if not os.path.isdir(FILES_DIR):
        print(f"received_files 目录不存在: {FILES_DIR}")
        return

    files = sorted(
        f for f in os.listdir(FILES_DIR) if os.path.isfile(os.path.join(FILES_DIR, f))
    )
    if not files:
        print("(空 —— 还没有收到任何文件)")
        return

    print(f"共 {len(files)} 个文件\n")
    for name in files:
        path = os.path.join(FILES_DIR, name)
        size = os.path.getsize(path)
        record_name = _find_matching_record(name)

        # 判断文件类型
        ext = os.path.splitext(name)[1].lower()
        if ext in (".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp"):
            ftype = "🖼 图片"
        elif ext in (".mp4", ".avi", ".mov", ".mkv", ".webm"):
            ftype = "🎬 视频"
        else:
            ftype = "📄 未知"

        print(f"  {name}")
        print(f"    类型: {ftype}  大小: {_human_size(size)}")
        if record_name:
            print(f"    对应请求: {record_name}")

        if detail:
            if ftype == "🖼 图片":
                info = _get_image_info(path) or _guess_image_info(path)
                if info:
                    print(f"    图片信息: {info}")
            elif ftype == "🎬 视频":
                info = _get_video_info(path)
                if info:
                    for k, v in info.items():
                        print(f"    {k}: {v}")

        print()


def open_file(pattern: str):
    """用系统工具打开匹配的文件。"""
    if not os.path.isdir(FILES_DIR):
        print(f"received_files 目录不存在")
        return

    matches = [f for f in os.listdir(FILES_DIR) if pattern in f]
    if not matches:
        print(f"没有匹配 '{pattern}' 的文件")
        return
    if len(matches) > 1:
        print(f"匹配多个文件: {matches}")
        return

    path = os.path.join(FILES_DIR, matches[0])
    ext = os.path.splitext(path)[1].lower()

    if ext in (".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp"):
        cmd = f'feh "{path}" 2>/dev/null || xdg-open "{path}" 2>/dev/null || echo "请手动打开: {path}"'
    elif ext in (".mp4", ".avi", ".mov", ".mkv", ".webm"):
        cmd = f'ffplay "{path}" 2>/dev/null || xdg-open "{path}" 2>/dev/null || echo "请手动打开: {path}"'
    else:
        print(f"未知文件类型，路径: {path}")
        return

    os.system(cmd)


def main():
    parser = argparse.ArgumentParser(description="检查 Dify 测试服务器收到的文件")
    parser.add_argument("--detail", action="store_true", help="显示详细信息（尺寸、时长等）")
    parser.add_argument("--open", type=str, metavar="PATTERN", help="打开匹配的文件（如 seq0001）")
    args = parser.parse_args()

    if args.open:
        open_file(args.open)
    else:
        list_files(detail=args.detail)


if __name__ == "__main__":
    main()
