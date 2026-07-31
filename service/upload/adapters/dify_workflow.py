import mimetypes
import os
from typing import Any, Dict

import requests

from .base import DeliveryAdapter, DeliveryResult
from .mapping import mapped_values, set_path


class DifyWorkflowAdapter(DeliveryAdapter):
    adapter_id = "dify_workflow"

    @staticmethod
    def _base_url(value: str) -> str:
        value = value.rstrip("/")
        for suffix in ("/v1/files/upload", "/v1/workflows/run"):
            if value.endswith(suffix):
                return value[:-len(suffix)]
        return value

    def _connection(self):
        api_url = str(self.profile.get("api_url", "")).strip()
        api_key = str(self.profile.get("api_key", "")).strip()
        if not api_url or not api_key:
            raise ValueError("Dify api_url/api_key is empty")
        return self._base_url(api_url), api_key, float(self.profile.get("timeout", 120))

    @staticmethod
    def _file_type(path: str) -> str:
        mime = mimetypes.guess_type(path)[0] or ""
        if mime.startswith("image/"):
            return "image"
        if mime.startswith("video/"):
            return "video"
        return "custom"

    def _mapped_inputs(self, event: Dict[str, Any], delivery: Dict[str, Any], preview: bool):
        inputs, files = mapped_values(event, delivery.get("mapping", []), preview=preview)
        if preview:
            for target, path in files.items():
                set_path(inputs, target, {"file": os.path.basename(path)})
        return inputs, files

    def preview(self, event_dir: str, event: Dict[str, Any], delivery: Dict[str, Any]) -> Dict[str, Any]:
        base_url, _api_key, _timeout = self._connection()
        inputs, _files = self._mapped_inputs(event, delivery, True)
        return {
            "adapter": self.adapter_id,
            "method": "POST",
            "upload_url": f"{base_url}/v1/files/upload",
            "workflow_url": f"{base_url}/v1/workflows/run",
            "inputs": inputs,
        }

    def send(self, event_dir: str, event: Dict[str, Any], delivery: Dict[str, Any]) -> DeliveryResult:
        base_url, api_key, timeout = self._connection()
        inputs, files = self._mapped_inputs(event, delivery, False)
        user = str(delivery.get("user") or f"rk3588-ch{event['source'].get('channel_id', 0)}")
        for target, path in files.items():
            with open(path, "rb") as stream:
                mime = mimetypes.guess_type(path)[0] or "application/octet-stream"
                response = requests.post(
                    f"{base_url}/v1/files/upload",
                    files={"file": (os.path.basename(path), stream, mime)},
                    data={"user": user},
                    headers={"Authorization": f"Bearer {api_key}"},
                    timeout=timeout,
                )
            if response.status_code != 201:
                return DeliveryResult(
                    False, f"Dify file upload failed: HTTP {response.status_code}",
                    response.status_code, response.text[:2000],
                )
            file_id = str(response.json().get("id", ""))
            if not file_id:
                return DeliveryResult(False, "Dify file upload response has no id", response.status_code)
            value = {
                "type": self._file_type(path),
                "transfer_method": "local_file",
                "upload_file_id": file_id,
            }
            mode = next(
                (str(item.get("file_mode", "single"))
                 for item in delivery.get("mapping", [])
                 if isinstance(item, dict) and str(item.get("target", "")).strip() == target),
                "single",
            )
            set_path(inputs, target, [value] if mode == "list" else value)

        event_id = event.get("event", {}).get("id", "unknown")
        delivery_id = delivery.get("id", "unknown")
        payload = {
            "inputs": inputs, "response_mode": "blocking", "user": user,
            "workflow_run_id": f"{event_id}:{delivery_id}",
        }
        response = requests.post(
            f"{base_url}/v1/workflows/run",
            json=payload,
            headers={"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"},
            timeout=timeout,
        )
        try:
            response_body: Any = response.json()
        except ValueError:
            response_body = response.text[:2000]
        ok = response.status_code == 200
        terminal = False
        if ok and isinstance(response_body, dict):
            data = response_body.get("data", {})
            if isinstance(data, dict) and data.get("status") == "failed":
                ok = False
                terminal = True
        detail = "delivered" if ok else (
            f"Dify workflow task failed (terminal): HTTP {response.status_code}" if terminal
            else f"Dify workflow failed: HTTP {response.status_code}"
        )
        result = DeliveryResult(ok, detail, response.status_code, response_body)
        result.terminal = terminal
        return result
