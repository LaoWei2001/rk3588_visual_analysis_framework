#!/usr/bin/env python3
"""
解码 received/ 目录中 JSON 文件里的 Base64 图片，生成真实图片文件。

用法:
    python3 decode_images.py                # 处理 received/ 下所有 JSON
    python3 decode_images.py req_xxx.json   # 只处理指定文件

输出目录: decoded_images/<请求文件名>/base64Data.jpg, base64DataRaw.jpg 等
"""

import base64
import json
import os
import sys

RECEIVED_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "received")
OUTPUT_ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "decoded_images")

# 常见的 Base64 图片字段名
BASE64_IMAGE_KEYS = (
    "base64Data",
    "base64DataRaw",
    "base64Image",
    "image",
    "snapshot",
    "picture",
)


def looks_like_base64(value: str) -> bool:
    """快速判断字符串是否像 Base64 编码（长度合理、合法字符）。"""
    if not isinstance(value, str) or len(value) < 64:
        return False
    allowed = set(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/="
    )
    sample = value[:200]
    return all(c in allowed for c in sample)


def decode_body(obj, output_dir: str):
    """递归扫描 JSON 对象，找到 Base64 字符串并解码为文件。"""
    count = 0
    if isinstance(obj, dict):
        for key, value in obj.items():
            if isinstance(value, str) and looks_like_base64(value):
                filename = os.path.join(output_dir, f"{key}.jpg")
                try:
                    data = base64.b64decode(value)
                    with open(filename, "wb") as f:
                        f.write(data)
                    size_kb = len(data) / 1024
                    print(f"  {key} -> {filename} ({size_kb:.1f} KB)")
                    count += 1
                except Exception as exc:
                    print(f"  {key} -> 解码失败: {exc}")
            elif isinstance(value, (dict, list)):
                count += decode_body(value, output_dir)
    elif isinstance(obj, list):
        for item in obj:
            count += decode_body(item, output_dir)
    return count


def process_file(json_path: str):
    basename = os.path.splitext(os.path.basename(json_path))[0]
    output_dir = os.path.join(OUTPUT_ROOT, basename)
    os.makedirs(output_dir, exist_ok=True)

    with open(json_path, "r", encoding="utf-8") as f:
        data = json.load(f)

    # 支持 server.py 保存的格式（body 里是上报 JSON）
    body = data.get("body") if isinstance(data, dict) else data

    print(f"处理: {os.path.basename(json_path)}", flush=True)
    count = decode_body(body, output_dir)
    if count == 0:
        print(f"  未找到 Base64 图片数据", flush=True)
    else:
        print(f"  共解码 {count} 张图片", flush=True)


def main():
    if len(sys.argv) > 1:
        files = []
        for arg in sys.argv[1:]:
            path = os.path.join(RECEIVED_DIR, arg) if not os.path.isabs(arg) else arg
            if os.path.isfile(path):
                files.append(path)
            else:
                print(f"文件不存在: {path}", flush=True)
    else:
        if not os.path.isdir(RECEIVED_DIR):
            print(f"received 目录不存在: {RECEIVED_DIR}", flush=True)
            return
        files = sorted(
            os.path.join(RECEIVED_DIR, name)
            for name in os.listdir(RECEIVED_DIR)
            if name.endswith(".json")
        )

    if not files:
        print("没有找到 JSON 文件", flush=True)
        return

    os.makedirs(OUTPUT_ROOT, exist_ok=True)
    print(f"输出目录: {OUTPUT_ROOT}", flush=True)
    print(f"共 {len(files)} 个文件", flush=True)
    print(f"{'='*50}", flush=True)

    for path in files:
        process_file(path)

    print(f"{'='*50}", flush=True)
    print("完成", flush=True)


if __name__ == "__main__":
    main()
