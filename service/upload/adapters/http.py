import json
import mimetypes
import os
from typing import Any, Dict
from urllib.parse import urljoin

import requests

from .base import DeliveryAdapter, DeliveryResult, is_retryable_http_status
from .mapping import MISSING, mapped_parts, response_path


class HttpAdapter(DeliveryAdapter):
    adapter_id = "http"

    def _request(self, event: Dict[str, Any], delivery: Dict[str, Any], preview: bool):
        base_url = str(self.connection.get("base_url", "")).strip().rstrip("/") + "/"
        if base_url == "/":
            raise ValueError("HTTP connection base_url is empty")
        request_options = delivery.get("request", {})
        if not isinstance(request_options, dict):
            raise ValueError("contract request must be an object")
        path = str(request_options.get("path", "")).strip().lstrip("/")
        if not path:
            raise ValueError("HTTP contract request.path is empty")
        method = str(request_options.get("method", "POST")).upper()
        if method not in ("POST", "PUT", "PATCH"):
            raise ValueError("HTTP method must be POST, PUT or PATCH")
        encoding = str(request_options.get("encoding", "json")).strip()
        if encoding not in ("json", "form", "multipart"):
            raise ValueError("HTTP encoding must be json, form or multipart")
        headers = self.connection.get("headers", {})
        if not isinstance(headers, dict):
            raise ValueError("connection headers must be an object")
        parts = mapped_parts(event, delivery.get("mapping", []), preview=preview)
        headers = {str(key): str(value) for key, value in headers.items()}
        headers.update({str(key): str(value) for key, value in parts["header"].items()})
        headers.setdefault("X-Idempotency-Key", f"{event['event'].get('id', '')}:{delivery.get('id', '')}")
        json_part = str(request_options.get("json_part", "")).strip()
        return method, urljoin(base_url, path), encoding, json_part, headers, parts

    def preview(self, event_dir: str, event: Dict[str, Any], delivery: Dict[str, Any]) -> Dict[str, Any]:
        method, url, encoding, json_part, headers, parts = self._request(event, delivery, True)
        secret_words = ("authorization", "token", "api-key", "apikey", "secret")
        safe_headers = {
            key: ("***" if any(word in key.lower() for word in secret_words) else value)
            for key, value in headers.items()
        }
        return {
            "adapter": self.adapter_id,
            "method": method,
            "url": url,
            "encoding": encoding,
            "json_part": json_part or None,
            "headers": safe_headers,
            "query": parts["query"],
            "body": parts["body"],
            "form": parts["form"],
            "files": {key: os.path.basename(str(path)) for key, path in parts["file"].items()},
        }

    def send(self, event_dir: str, event: Dict[str, Any], delivery: Dict[str, Any]) -> DeliveryResult:
        method, url, encoding, json_part, headers, parts = self._request(event, delivery, False)
        timeout = float(self.connection.get("timeout", 15))
        opened = []
        kwargs: Dict[str, Any] = {
            "method": method, "url": url, "params": parts["query"],
            "headers": headers, "timeout": timeout,
        }
        try:
            if encoding == "json":
                if parts["file"] or parts["form"]:
                    raise ValueError("json request cannot contain form/file mappings")
                kwargs["json"] = parts["body"]
            elif encoding == "form":
                if parts["file"] or parts["body"]:
                    raise ValueError("form request only accepts form/query/header mappings")
                kwargs["data"] = parts["form"]
            else:
                files = {}
                for key, path in parts["file"].items():
                    stream = open(str(path), "rb")
                    opened.append(stream)
                    mime = mimetypes.guess_type(str(path))[0] or "application/octet-stream"
                    files[key] = (os.path.basename(str(path)), stream, mime)
                if parts["body"]:
                    if not json_part:
                        raise ValueError("multipart body mappings require request.json_part")
                    if json_part in files or json_part in parts["form"]:
                        raise ValueError("multipart json_part conflicts with a form/file field")
                    files[json_part] = (
                        None, json.dumps(parts["body"], ensure_ascii=False), "application/json",
                    )
                if not files:
                    raise ValueError("multipart request requires a file or JSON body part")
                kwargs["data"] = parts["form"]
                kwargs["files"] = files
            response = requests.request(**kwargs)
        finally:
            for stream in opened:
                stream.close()

        success = delivery.get("success", {})
        success = success if isinstance(success, dict) else {}
        statuses = success.get("http_status", [200, 201, 202, 204])
        if not isinstance(statuses, list):
            raise ValueError("success.http_status must be an array")
        ok = response.status_code in [int(value) for value in statuses]
        try:
            parsed: Any = response.json()
        except ValueError:
            parsed = response.text[:2000]
        path = str(success.get("json_path", "")).strip()
        if ok and path:
            actual = response_path(parsed, path)
            ok = actual is not MISSING and actual == success.get("equals")
        detail = "delivered" if ok else f"remote rejected request: HTTP {response.status_code}"
        terminal = not ok and 400 <= response.status_code < 500 and not is_retryable_http_status(response.status_code)
        return DeliveryResult(ok, detail, response.status_code, parsed, terminal)
