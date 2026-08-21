#!/usr/bin/env python3
"""
Dify 快速联调脚本 —— 把本地图片/视频直接上报到自己的 Dify 工作流。

不依赖 C++ 主程序、事件链路或上传服务，仅需 `pip install requests`。
适合二次开发者在配置接口契约之前，先验证自己 Dify 工作流的输入变量名、
文件变量类型和最终输出。

流程（与正式 DifyWorkflowAdapter 相同）：
  1. POST {api_url}/v1/files/upload   逐个上传本地文件，取得 file_id
  2. POST {api_url}/v1/workflows/run  以 local_file 引用提交工作流，blocking 等待结果

示例：
  # 只发一张图片 + 提示词（工作流变量名默认 image）
  python3 dify_send_test.py \\
    --api-url http://your-dify.example.com \\
    --api-key app-xxxxxxxx \\
    --image ./snapshot.jpg \\
    --inputs '{"prompt": "请分析图片"}'

  # 图片 + 视频（工作流变量名默认 image / video）
  python3 dify_send_test.py \\
    --api-url http://your-dify.example.com \\
    --api-key app-xxxxxxxx \\
    --image ./snapshot.jpg \\
    --video ./clip.mp4 \\
    --inputs '{"prompt": "请分析视频"}'

  # 自定义变量名 / 文件列表变量（对应 Dify 文件列表类型）
  python3 dify_send_test.py \\
    --api-url http://your-dify.example.com \\
    --api-key app-xxxxxxxx \\
    --file ./a.jpg=snapshot \\
    --file ./b.jpg=snapshot \\
    --list \\
    --inputs '{"event_json": "..."}'
"""

import argparse
import json
import mimetypes
import os
import sys
import uuid

import requests


def _normalize_base_url(value: str) -> str:
    value = value.strip().rstrip("/")
    for suffix in ("/v1/files/upload", "/v1/workflows/run", "/v1"):
        if value.endswith(suffix):
            return value[: -len(suffix)]
    return value


def _file_type(path: str) -> str:
    mime = mimetypes.guess_type(path)[0] or ""
    if mime.startswith("image/"):
        return "image"
    if mime.startswith("video/"):
        return "video"
    return "custom"


def _upload_file(session, base_url, api_key, user, timeout, path):
    with open(path, "rb") as stream:
        mime = mimetypes.guess_type(path)[0] or "application/octet-stream"
        response = session.post(
            f"{base_url}/v1/files/upload",
            files={"file": (os.path.basename(path), stream, mime)},
            data={"user": user},
            headers={"Authorization": f"Bearer {api_key}"},
            timeout=timeout,
        )
    if response.status_code != 201:
        print(f"  上传失败: HTTP {response.status_code}\n{response.text[:2000]}")
        return None
    try:
        body = response.json()
    except ValueError:
        print(f"  上传失败: 响应不是 JSON\n{response.text[:2000]}")
        return None
    file_id = str(body.get("id", ""))
    if not file_id:
        print(f"  上传失败: 响应缺少 id 字段: {json.dumps(body, ensure_ascii=False)}")
        return None
    return file_id


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--api-url", required=True,
                        help="Dify 服务地址，如 http://192.168.1.10（自动去掉 /v1 等后缀）")
    parser.add_argument("--api-key", required=True,
                        help="Dify 应用 API Key（app- 开头）")
    parser.add_argument("--image", action="append", default=[], metavar="PATH",
                        help="本地图片，发送到工作流变量 image；可多次指定")
    parser.add_argument("--video", action="append", default=[], metavar="PATH",
                        help="本地视频，发送到工作流变量 video；可多次指定")
    parser.add_argument("--file", action="append", default=[], metavar="PATH=TARGET",
                        help="发送到任意变量名，如 ./a.jpg=snapshot；可多次指定")
    parser.add_argument("--inputs", default="{}", metavar="JSON",
                        help='其余工作流输入，JSON 对象，如 \'{"prompt": "请分析"}\'')
    parser.add_argument("--user", default="rk3588-quick-test",
                        help="Dify user 标识（默认 rk3588-quick-test）")
    parser.add_argument("--timeout", type=float, default=120,
                        help="单次请求超时秒数（默认 120）")
    parser.add_argument("--workflow-run-id", default="",
                        help="自定义 workflow_run_id（默认不发送该字段）")
    parser.add_argument("--list", dest="list_mode", action="store_true",
                        help="文件变量按文件列表格式发送（对应 Dify 文件列表变量）")
    args = parser.parse_args()

    base_url = _normalize_base_url(args.api_url)

    try:
        inputs = json.loads(args.inputs)
    except ValueError as exc:
        print(f"--inputs 不是合法 JSON: {exc}")
        return 2
    if not isinstance(inputs, dict):
        print("--inputs 必须是 JSON 对象")
        return 2

    file_targets = []
    for path in args.image:
        file_targets.append((path, "image"))
    for path in args.video:
        file_targets.append((path, "video"))
    for spec in args.file:
        if "=" not in spec:
            print(f"--file 需要 PATH=TARGET 格式: {spec}")
            return 2
        path, target = spec.split("=", 1)
        file_targets.append((path.strip(), target.strip()))

    if not file_targets:
        print("没有指定任何文件，使用 --image / --video / --file 指定")
        return 2
    for path, target in file_targets:
        if not os.path.isfile(path):
            print(f"文件不存在: {path}")
            return 2
        if not target:
            print(f"--file 的变量名为空: {path}={target}")
            return 2

    groups = {}
    for path, target in file_targets:
        groups.setdefault(target, []).append(path)

    session = requests.Session()

    print(f"\n[1/2] 上传文件 -> {base_url}/v1/files/upload")
    file_ids = {}
    for target, paths in groups.items():
        for path in paths:
            size_kb = os.path.getsize(path) / 1024
            size_str = f"{size_kb:.1f} KB" if size_kb < 1024 else f"{size_kb / 1024:.2f} MB"
            print(f"  上传 {target} <- {os.path.basename(path)} ({size_str}) ...")
            fid = _upload_file(session, base_url, args.api_key, args.user, args.timeout, path)
            if not fid:
                return 1
            file_ids.setdefault(target, []).append(fid)
            print(f"    -> file_id={fid}")

    for target, ids in file_ids.items():
        refs = [
            {
                "type": _file_type(path),
                "transfer_method": "local_file",
                "upload_file_id": fid,
            }
            for path, fid in zip(groups[target], ids)
        ]
        inputs[target] = refs if (args.list_mode or len(refs) > 1) else refs[0]

    payload = {
        "inputs": inputs,
        "response_mode": "blocking",
        "user": args.user,
    }
    if args.workflow_run_id:
        payload["workflow_run_id"] = args.workflow_run_id

    print(f"\n[2/2] 运行工作流 -> {base_url}/v1/workflows/run")
    print(f"  user={args.user}")
    print(f"  inputs:")
    print(json.dumps(inputs, ensure_ascii=False, indent=4))

    response = session.post(
        f"{base_url}/v1/workflows/run",
        json=payload,
        headers={"Authorization": f"Bearer {args.api_key}", "Content-Type": "application/json"},
        timeout=args.timeout,
    )
    try:
        body = response.json()
    except ValueError:
        body = response.text[:2000]

    if response.status_code != 200:
        print(f"\n工作流调用失败: HTTP {response.status_code}")
        print(body)
        return 1
    if isinstance(body, dict):
        data = body.get("data", {})
        if isinstance(data, dict) and data.get("status") == "failed":
            print("\n工作流执行失败:")
            print(json.dumps(data, ensure_ascii=False, indent=2))
            return 1

    print("\n工作流执行成功:")
    print(json.dumps(body, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
