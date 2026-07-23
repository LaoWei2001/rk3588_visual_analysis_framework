"""Dify 文件上传与工作流调用客户端。"""

import base64
import io
import mimetypes
import os
from typing import Any, Dict

import requests


class DifyUploader:
    def __init__(self, config: Dict[str, Any]):
        dify = config.get("dify", {})
        self.default_api_url = str(dify.get("api_url", ""))
        self.default_api_key = str(dify.get("api_key", ""))
        self.timeout = int(dify.get("timeout", 120))
        self.last_upload_metadata: Dict[str, Any] = {}
        self.last_upload_metadata_list: list[Dict[str, Any]] = []
        self.last_workflow_result: Dict[str, Any] = {}

    @staticmethod
    def _derive_base(api_url: str) -> str:
        value = str(api_url).rstrip("/")
        for suffix in ("/v1/files/upload", "/v1/workflows/run"):
            if value.endswith(suffix):
                return value[:-len(suffix)]
        return value

    def _upload_image(self, image_b64: str, user: str, url: str, api_key: str) -> str:
        image = base64.b64decode(image_b64)
        response = requests.post(
            url,
            files={"file": ("alarm.jpg", io.BytesIO(image), "image/jpeg")},
            data={"user": user},
            headers={"Authorization": f"Bearer {api_key}"},
            timeout=self.timeout,
        )
        if response.status_code != 201:
            print(f"[Dify] image upload failed: status={response.status_code} body={response.text}")
            return ""
        self.last_upload_metadata = dict(response.json())
        return str(self.last_upload_metadata.get("id", ""))

    def _upload_video(self, path: str, user: str, url: str, api_key: str) -> str:
        if not os.path.isfile(path):
            return ""
        with open(path, "rb") as stream:
            response = requests.post(
                url,
                files={"file": (os.path.basename(path), stream, "video/mp4")},
                data={"user": user},
                headers={"Authorization": f"Bearer {api_key}"},
                timeout=self.timeout,
            )
        if response.status_code != 201:
            print(f"[Dify] video upload failed: status={response.status_code} body={response.text}")
            return ""
        self.last_upload_metadata = dict(response.json())
        return str(self.last_upload_metadata.get("id", ""))

    def _upload_path(
        self, path: str, file_type: str, user: str, url: str, api_key: str
    ) -> str:
        """上传磁盘中的图片或 MP4，供多媒体列表上报复用。"""
        if not os.path.isfile(path):
            print(f"[Dify] local file not found: {path}")
            return ""
        suffix = os.path.splitext(path)[1].lower()
        if file_type == "video":
            if suffix != ".mp4":
                print(f"[Dify] unsupported video extension: {suffix or '(none)'}")
                return ""
            mime_type = "video/mp4"
        elif file_type == "image":
            mime_type = mimetypes.guess_type(path)[0] or ""
            if not mime_type.startswith("image/"):
                print(f"[Dify] unsupported image extension: {suffix or '(none)'}")
                return ""
        else:
            print(f"[Dify] unsupported local media type: {file_type}")
            return ""

        with open(path, "rb") as stream:
            response = requests.post(
                url,
                files={"file": (os.path.basename(path), stream, mime_type)},
                data={"user": user},
                headers={"Authorization": f"Bearer {api_key}"},
                timeout=self.timeout,
            )
        if response.status_code != 201:
            print(
                f"[Dify] {file_type} upload failed: "
                f"status={response.status_code} body={response.text}"
            )
            return ""
        self.last_upload_metadata = dict(response.json())
        return str(self.last_upload_metadata.get("id", ""))

    def _run_workflow(
        self, base_url: str, api_key: str, user: str, inputs: Dict[str, Any]
    ) -> bool:
        workflow_payload = {
            "inputs": inputs,
            "files": [],
            "response_mode": "blocking",
            "user": user,
        }
        response = requests.post(
            f"{base_url}/v1/workflows/run",
            json=workflow_payload,
            headers={"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"},
            timeout=self.timeout,
        )
        if response.status_code != 200:
            metadata = self.last_upload_metadata_list or (
                [self.last_upload_metadata] if self.last_upload_metadata else []
            )
            if metadata:
                safe_metadata = [
                    {key: item.get(key) for key in ("id", "name", "size", "extension", "mime_type")}
                    for item in metadata
                ]
                print(f"[Dify] uploaded file metadata: {safe_metadata}")
            print(f"[Dify] workflow failed: status={response.status_code} body={response.text}")
            return False
        result = response.json()
        self.last_workflow_result = dict(result)
        if result.get("data", {}).get("status") == "failed":
            print(f"[Dify] workflow execution failed: {result.get('data', {}).get('error', '')}")
            return False
        return True

    def upload(self, data: Dict[str, Any]) -> bool:
        self.last_upload_metadata = {}
        self.last_upload_metadata_list = []
        self.last_workflow_result = {}
        file_type = str(data.get("file_type", "image"))
        image_b64 = str(data.get("base64Data", ""))
        video_path = str(data.get("file_path", ""))
        if not image_b64 and not video_path:
            return False

        try:
            api_url = str(data.get("dify_api_url") or self.default_api_url)
            api_key = str(data.get("dify_api_key") or self.default_api_key)
            if not api_url or not api_key:
                raise ValueError("Dify api_url/api_key missing")
            base_url = self._derive_base(api_url)
            user = str(data.get("user", "rk3588"))
            file_id = (
                self._upload_video(video_path, user, f"{base_url}/v1/files/upload", api_key)
                if file_type == "video"
                else self._upload_image(image_b64, user, f"{base_url}/v1/files/upload", api_key)
            )
            if not file_id:
                return False

            inputs = dict(data.get("inputs", {}))
            # file_type 决定读取图片还是视频；dify_file_type 对应 Dify 开始节点允许的
            # image/video/document/audio/custom 类型。通用 File 节点可能把 MP4 配成 custom。
            dify_file_type = str(data.get("dify_file_type") or file_type).strip().lower()
            if dify_file_type not in ("image", "video", "document", "audio", "custom"):
                raise ValueError(f"unsupported dify_file_type: {dify_file_type}")
            file_input = {
                "type": dify_file_type,
                "transfer_method": "local_file",
                "upload_file_id": file_id,
            }
            variable = str(data.get("input_variable") or ("video" if file_type == "video" else "image"))
            # Dify 开始节点同时支持 File 和 Array[File]。旧实现把视频固定包装成数组，
            # 当工作流变量是单文件时会报 "must be a file"。调用方现在可显式选择。
            default_mode = "list" if file_type == "video" else "single"
            input_mode = str(data.get("file_input_mode") or default_mode).strip().lower()
            if input_mode not in ("single", "list"):
                raise ValueError(f"unsupported file_input_mode: {input_mode}")
            inputs[variable] = [file_input] if input_mode == "list" else file_input
            if "prompt" in data and "prompt" not in inputs:
                inputs["prompt"] = data["prompt"]
            if "event_id" in data and "event_id" not in inputs:
                inputs["event_id"] = data["event_id"]

            # files=[] 兼容仍保留旧版顶层 files 字段的 Dify 1.x；真正的开始节点
            # 文件变量始终位于 inputs 中。
            return self._run_workflow(base_url, api_key, user, inputs)
        except Exception as exc:
            print(f"[Dify] upload failed: {exc}")
            return False

    def run_workflow(self, data: Dict[str, Any]) -> bool:
        """不上传文件，只用业务 JSON 和映射参数执行一次 Dify 工作流。"""
        self.last_upload_metadata = {}
        self.last_upload_metadata_list = []
        self.last_workflow_result = {}
        try:
            api_url = str(data.get("dify_api_url") or self.default_api_url)
            api_key = str(data.get("dify_api_key") or self.default_api_key)
            if not api_url or not api_key:
                raise ValueError("Dify api_url/api_key missing")
            base_url = self._derive_base(api_url)
            user = str(data.get("user", "rk3588"))
            inputs = dict(data.get("inputs", {}))
            if "prompt" in data and "prompt" not in inputs:
                inputs["prompt"] = data["prompt"]
            if "event_id" in data and "event_id" not in inputs:
                inputs["event_id"] = data["event_id"]
            return self._run_workflow(base_url, api_key, user, inputs)
        except Exception as exc:
            print(f"[Dify] workflow failed: {exc}")
            return False

    def upload_media_list(self, data: Dict[str, Any]) -> bool:
        """上传多个图片/视频，并作为同一个 Dify File list 执行一次工作流。"""
        self.last_upload_metadata = {}
        self.last_upload_metadata_list = []
        self.last_workflow_result = {}
        try:
            api_url = str(data.get("dify_api_url") or self.default_api_url)
            api_key = str(data.get("dify_api_key") or self.default_api_key)
            if not api_url or not api_key:
                raise ValueError("Dify api_url/api_key missing")
            base_url = self._derive_base(api_url)
            user = str(data.get("user", "rk3588"))
            media = data.get("media", [])
            if not isinstance(media, list) or not media:
                raise ValueError("media must be a non-empty list")

            file_inputs = []
            for item in media:
                if not isinstance(item, dict):
                    raise ValueError("each media item must be an object")
                file_type = str(item.get("file_type", "")).strip().lower()
                file_path = str(item.get("file_path", "")).strip()
                dify_file_type = str(item.get("dify_file_type") or file_type).strip().lower()
                if dify_file_type not in ("image", "video", "custom"):
                    raise ValueError(f"unsupported Dify media type: {dify_file_type}")
                file_id = self._upload_path(
                    file_path,
                    file_type,
                    user,
                    f"{base_url}/v1/files/upload",
                    api_key,
                )
                if not file_id:
                    return False
                self.last_upload_metadata_list.append(dict(self.last_upload_metadata))
                file_inputs.append({
                    "type": dify_file_type,
                    "transfer_method": "local_file",
                    "upload_file_id": file_id,
                })

            variable = str(data.get("input_variable") or "media_files").strip()
            if not variable:
                raise ValueError("input_variable is empty")
            inputs = dict(data.get("inputs", {}))
            inputs[variable] = file_inputs
            return self._run_workflow(base_url, api_key, user, inputs)
        except Exception as exc:
            print(f"[Dify] media list upload failed: {exc}")
            return False
