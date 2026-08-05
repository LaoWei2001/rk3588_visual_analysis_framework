import json
import mimetypes
import os
from typing import Any, Dict

import requests

from .base import DeliveryAdapter, DeliveryResult
from .mapping import MISSING, mapped_values, response_path


class HttpJsonAdapter(DeliveryAdapter):
    adapter_id = "http_json"

    def _request(self, event: Dict[str, Any], delivery: Dict[str, Any], preview: bool):
        url = str(self.profile.get("url", "")).strip()
        if not url:
            raise ValueError("profile url is empty")
        request_options = delivery.get("request", {})
        if not isinstance(request_options, dict):
            raise ValueError("contract request must be an object")
        method = str(request_options.get("method", "POST")).upper()
        if method not in ("POST", "PUT", "PATCH"):
            raise ValueError("HTTP method must be POST, PUT or PATCH")
        headers = self.profile.get("headers", {})
        if not isinstance(headers, dict):
            raise ValueError("profile headers must be an object")
        headers = {str(key): str(value) for key, value in headers.items()}
        headers.setdefault("X-Idempotency-Key", f"{event['event'].get('id', '')}:{delivery.get('id', '')}")
        body, files = mapped_values(event, delivery.get("mapping", []), preview=preview)
        return method, url, headers, body, files

    def preview(self, event_dir: str, event: Dict[str, Any], delivery: Dict[str, Any]) -> Dict[str, Any]:
        method, url, headers, body, files = self._request(event, delivery, True)
        secret_words = ("authorization", "token", "api-key", "apikey", "secret")
        safe_headers = {
            key: ("***" if any(word in key.lower() for word in secret_words) else value)
            for key, value in headers.items()
        }
        return {
            "adapter": self.adapter_id,
            "method": method,
            "url": url,
            "headers": safe_headers,
            "body": body,
            "files": {key: os.path.basename(path) for key, path in files.items()},
        }

    def send(self, event_dir: str, event: Dict[str, Any], delivery: Dict[str, Any]) -> DeliveryResult:
        method, url, headers, body, file_paths = self._request(event, delivery, False)
        timeout = float(self.profile.get("timeout", 15))
        opened = []
        try:
            if file_paths:
                files = {}
                for key, path in file_paths.items():
                    stream = open(path, "rb")
                    opened.append(stream)
                    mime = mimetypes.guess_type(path)[0] or "application/octet-stream"
                    files[key] = (os.path.basename(path), stream, mime)
                response = requests.request(
                    method, url, data={"payload": json.dumps(body, ensure_ascii=False)},
                    files=files, headers=headers, timeout=timeout,
                )
            else:
                response = requests.request(method, url, json=body, headers=headers, timeout=timeout)
        finally:
            for stream in opened:
                stream.close()

        success = delivery.get("success", {})
        success = success if isinstance(success, dict) else {}
        statuses = success.get("http_status", [200, 201, 202, 204])
        if not isinstance(statuses, list):
            raise ValueError("success.http_status must be an array")
        ok = response.status_code in [int(value) for value in statuses]
        parsed: Any = None
        try:
            parsed = response.json()
        except ValueError:
            parsed = response.text[:2000]
        path = str(success.get("json_path", "")).strip()
        if ok and path:
            actual = response_path(parsed, path)
            ok = actual is not MISSING and actual == success.get("equals")
        detail = "delivered" if ok else f"remote rejected request: HTTP {response.status_code}"
        return DeliveryResult(ok, detail, response.status_code, parsed)
