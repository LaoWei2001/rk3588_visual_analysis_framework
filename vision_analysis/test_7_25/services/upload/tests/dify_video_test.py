#!/usr/bin/env python3
"""使用生产环境 DifyUploader 验证“图片 + 视频 + 提示词”工作流。"""

import argparse
import json
import mimetypes
import sys
from pathlib import Path
from typing import Any, Dict, Tuple

import requests
import yaml


TEST_DIR = Path(__file__).resolve().parent
UPLOAD_DIR = TEST_DIR.parent
if str(UPLOAD_DIR) not in sys.path:
    sys.path.insert(0, str(UPLOAD_DIR))

from dify_uploader import DifyUploader  # noqa: E402


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="通过项目真实上传逻辑向 Dify 工作流同时发送图片、MP4 视频和提示词",
    )
    parser.add_argument("--video", required=True, help="要上传的 MP4 视频路径")
    parser.add_argument("--image", required=True, help="要与视频结合分析的图片路径")
    parser.add_argument("--prompt", required=True, help="传给 Dify 输入变量 prompt 的文本")
    parser.add_argument(
        "--config",
        default=str(TEST_DIR / "dify_video_test.yaml"),
        help="测试专用配置文件，默认使用 tests/dify_video_test.yaml",
    )
    return parser.parse_args()


def load_config(path: Path) -> Dict[str, Any]:
    if not path.is_file():
        raise FileNotFoundError(f"配置文件不存在: {path}")
    with path.open("r", encoding="utf-8") as stream:
        data = yaml.safe_load(stream) or {}
    if not isinstance(data, dict):
        raise ValueError("配置文件根节点必须是对象")
    dify = data.get("dify", {})
    if not isinstance(dify, dict):
        raise ValueError("测试配置缺少 dify 对象")

    # 测试配置显式拆分协议、地址和端口；最终转换成生产 DifyUploader 使用的 api_url。
    scheme = str(dify.get("scheme", "http")).strip().lower()
    host = str(dify.get("host", "")).strip()
    try:
        port = int(dify.get("port", 0))
    except (TypeError, ValueError) as exc:
        raise ValueError("dify.port 必须是整数") from exc
    if scheme not in ("http", "https"):
        raise ValueError("dify.scheme 只能是 http 或 https")
    if not host:
        raise ValueError("测试配置缺少 dify.host")
    if port < 1 or port > 65535:
        raise ValueError("dify.port 必须在 1~65535 之间")
    base_path = str(dify.get("base_path", "")).strip().strip("/")
    api_url = f"{scheme}://{host}:{port}"
    if base_path:
        api_url += f"/{base_path}"
    dify["api_url"] = api_url

    api_key = str(dify.get("api_key", "")).strip()
    if not api_key or "请替换" in api_key:
        raise ValueError("测试配置缺少 dify.api_key")
    data["dify"] = dify

    workflow = data.get("workflow", {})
    if not isinstance(workflow, dict):
        raise ValueError("测试配置中的 workflow 必须是对象")
    media_variable = str(workflow.get("media_variable", "media_files")).strip()
    prompt_variable = str(workflow.get("prompt_variable", "prompt")).strip()
    if not media_variable:
        raise ValueError("workflow.media_variable 不能为空")
    if not prompt_variable:
        raise ValueError("workflow.prompt_variable 不能为空")
    workflow["media_variable"] = media_variable
    workflow["prompt_variable"] = prompt_variable
    data["workflow"] = workflow
    return data


def find_input_field(parameters: Dict[str, Any], variable: str) -> Tuple[str, Dict[str, Any]]:
    """从 Dify /parameters 返回值中查找指定的开始节点变量。"""
    forms = parameters.get("user_input_form", [])
    if not isinstance(forms, list):
        return "", {}
    for form in forms:
        if not isinstance(form, dict):
            continue
        for field_kind, field in form.items():
            if isinstance(field, dict) and str(field.get("variable", "")) == variable:
                return str(field_kind), field
    return "", {}


def normalized_strings(value: Any) -> list[str]:
    if not isinstance(value, list):
        return []
    return [str(item).strip().lower() for item in value if str(item).strip()]


def inspect_workflow_media_input(
    dify: Dict[str, Any], media_variable: str, image_size: int, video_size: int
) -> bool:
    """校验已发布工作流是否允许一个 File list 同时接收图片和视频。"""
    base_url = DifyUploader._derive_base(str(dify["api_url"]))
    try:
        response = requests.get(
            f"{base_url}/v1/parameters",
            headers={"Authorization": f"Bearer {str(dify['api_key'])}"},
            timeout=int(dify.get("timeout", 120)),
        )
    except requests.RequestException as exc:
        print(f"[WARN] 无法读取 Dify 应用参数，将继续尝试上传: {exc}")
        return True
    if response.status_code != 200:
        print(
            f"[WARN] Dify /v1/parameters 返回 {response.status_code}，将继续尝试上传: "
            f"{response.text[:300]}"
        )
        return True

    try:
        parameters = response.json()
    except ValueError:
        print("[WARN] Dify /v1/parameters 未返回 JSON，将继续尝试上传")
        return True

    field_kind, field = find_input_field(parameters, media_variable)
    if not field:
        available = []
        for form in parameters.get("user_input_form", []):
            if isinstance(form, dict):
                for value in form.values():
                    if isinstance(value, dict) and value.get("variable"):
                        available.append(str(value["variable"]))
        print(
            f"[FAIL] 已发布工作流中找不到文件列表变量 {media_variable!r}；"
            f"当前变量: {', '.join(available) or '(无法读取)'}",
            file=sys.stderr,
        )
        return False

    print(f"  Dify变量: {media_variable} ({field_kind})")
    print(f"  Dify约束: {json.dumps(field, ensure_ascii=False, separators=(',', ':'))}")

    field_type = str(field.get("type", "")).strip().lower()
    kind_is_list = field_kind.lower() in ("file-list", "file_list", "array[file]") or field_type in (
        "file-list", "file_list", "array[file]"
    )
    if not kind_is_list:
        print(
            f"[FAIL] {media_variable} 必须是 Dify 的 File list/Array[File]，"
            f"当前是 {field_kind or field_type}",
            file=sys.stderr,
        )
        return False

    allowed_types = normalized_strings(field.get("allowed_file_types"))
    allowed_methods = normalized_strings(
        field.get("allowed_file_upload_methods", field.get("allowed_file_upload_method"))
    )

    if allowed_methods and "local_file" not in allowed_methods:
        print(
            "[FAIL] Dify 开始节点未允许本地文件上传(local_file)。"
            "请把该文件变量的上传方式改为“本地上传”或“两者”。",
            file=sys.stderr,
        )
        return False

    missing_types = [kind for kind in ("image", "video") if allowed_types and kind not in allowed_types]
    if missing_types:
        print(
            f"[FAIL] Dify 文件列表缺少允许类型 {missing_types}；"
            f"allowed_file_types={allowed_types}。请同时勾选图片和视频并重新发布。",
            file=sys.stderr,
        )
        return False

    try:
        max_length = int(field.get("max_length", 0) or 0)
    except (TypeError, ValueError):
        max_length = 0
    if max_length and max_length < 2:
        print(
            f"[FAIL] Dify 文件列表最大数量是 {max_length}，至少需要设置为 2",
            file=sys.stderr,
        )
        return False

    limits = parameters.get("system_parameters", {})
    if isinstance(limits, dict):
        size_checks = (
            ("image_file_size_limit", "图片", image_size),
            ("video_file_size_limit", "视频", video_size),
        )
        for limit_key, label, actual_size in size_checks:
            try:
                limit_mb = float(limits.get(limit_key, 0) or 0)
            except (TypeError, ValueError):
                limit_mb = 0
            if limit_mb > 0 and actual_size > limit_mb * 1024 * 1024:
                print(
                    f"[FAIL] {label}大小 {actual_size / 1024 / 1024:.2f} MB 超过 Dify 的 "
                    f"{limit_key}={limit_mb:g} MB",
                    file=sys.stderr,
                )
                return False

    print("  文件映射: 图片(image) + MP4(video) -> Dify File list")
    return True


def main() -> int:
    args = parse_args()
    video = Path(args.video).expanduser().resolve()
    image = Path(args.image).expanduser().resolve()
    config_path = Path(args.config).expanduser().resolve()
    prompt = str(args.prompt).strip()

    if not video.is_file():
        print(f"[FAIL] 视频文件不存在: {video}", file=sys.stderr)
        return 2
    if video.suffix.lower() != ".mp4":
        print("[FAIL] 当前生产上传逻辑按 video/mp4 发送，请使用 MP4 文件", file=sys.stderr)
        return 2
    if video.stat().st_size <= 0:
        print(f"[FAIL] 视频文件为空: {video}", file=sys.stderr)
        return 2
    if not image.is_file():
        print(f"[FAIL] 图片文件不存在: {image}", file=sys.stderr)
        return 2
    image_mime = mimetypes.guess_type(str(image))[0] or ""
    if not image_mime.startswith("image/"):
        print(f"[FAIL] 不支持的图片格式: {image.suffix or '(无扩展名)'}", file=sys.stderr)
        return 2
    if image.stat().st_size <= 0:
        print(f"[FAIL] 图片文件为空: {image}", file=sys.stderr)
        return 2
    if not prompt:
        print("[FAIL] --prompt 不能为空", file=sys.stderr)
        return 2

    try:
        config = load_config(config_path)
    except (OSError, ValueError) as exc:
        print(f"[FAIL] {exc}", file=sys.stderr)
        return 2

    dify = config["dify"]
    workflow = config["workflow"]
    media_variable = str(workflow["media_variable"])
    prompt_variable = str(workflow["prompt_variable"])
    print("[DifyMediaTest] 开始发送")
    print(f"  API:    {str(dify['api_url']).rstrip('/')}")
    print(f"  视频:   {video} ({video.stat().st_size / 1024 / 1024:.2f} MB)")
    print(f"  图片:   {image} ({image.stat().st_size / 1024 / 1024:.2f} MB)")
    print(f"  提示词: {prompt}")
    print(f"  inputs: {media_variable} (File list), {prompt_variable}")

    if not inspect_workflow_media_input(
        dify, media_variable, image.stat().st_size, video.stat().st_size
    ):
        return 1

    # 这里直接复用生产 DifyUploader：分别上传图片和视频，再把两个 upload_file_id
    # 放进同一个 inputs.<media_variable> 数组，最后只执行一次工作流。
    uploader = DifyUploader(config)
    ok = uploader.upload_media_list({
        "media": [
            {"file_type": "image", "file_path": str(image)},
            {"file_type": "video", "file_path": str(video)},
        ],
        "input_variable": media_variable,
        "inputs": {prompt_variable: prompt},
        "user": str(dify.get("user", "rk3588-dify-video-test")),
    })

    if not ok:
        print("[FAIL] Dify 文件上传或工作流执行失败；请查看上方 HTTP 状态和响应内容", file=sys.stderr)
        return 1

    print("[PASS] Dify 已接收图片和视频，并成功执行工作流")
    data = uploader.last_workflow_result.get("data", {})
    outputs = data.get("outputs", {}) if isinstance(data, dict) else {}
    print("[Dify回复]")
    if outputs:
        print(json.dumps(outputs, ensure_ascii=False, indent=2))
    else:
        print("(工作流执行成功，但输出节点没有返回任何 outputs 变量)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
