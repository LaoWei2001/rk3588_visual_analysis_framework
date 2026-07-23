"""统一待上报事件发件箱消费者，支持媒体告警和仅 JSON 的业务记录。"""

import base64
import json
import os
import shutil
import time
from threading import Event
from typing import Any, Dict

import requests

from dify_uploader import DifyUploader

_MISSING = object()


class MediaNotReadyError(RuntimeError):
    pass


class EventOutboxForwarder:
    SCAN_IDLE = 0.5
    MEDIA_WAIT = 0.2
    RETRY_WAIT = 10.0

    def __init__(self, config: Dict[str, Any], store_dir: str, dify: DifyUploader):
        self.store_dir = store_dir
        self.dify = dify
        self.server_url = str(config.get("server", {}).get("url", ""))
        self.server_timeout = int(config.get("server", {}).get("timeout", 30))
        profiles = config.get("profiles", {})
        self.profiles = profiles if isinstance(profiles, dict) else {}

    def _event_dirs(self):
        try:
            paths = [os.path.join(self.store_dir, name) for name in os.listdir(self.store_dir)]
        except FileNotFoundError:
            return []
        paths = [path for path in paths
                 if os.path.isdir(path) and os.path.isfile(os.path.join(path, "manifest.json"))]
        paths.sort(key=lambda path: os.path.getmtime(os.path.join(path, "manifest.json")))
        return paths

    @staticmethod
    def _read(path: str) -> Dict[str, Any]:
        with open(os.path.join(path, "manifest.json"), "r", encoding="utf-8") as stream:
            return json.load(stream)

    @staticmethod
    def _write(path: str, meta: Dict[str, Any]):
        final = os.path.join(path, "manifest.json")
        temporary = final + ".upload.tmp"
        # C++ 可能仍在补写视频文件名或合并触发信息。上传完成后落状态前重新合并这些
        # 生产者字段，避免图片 HTTP 请求期间把刚生成的视频信息覆盖掉。
        try:
            with open(final, "r", encoding="utf-8") as stream:
                current = json.load(stream)
            current_media = current.get("media", {})
            if isinstance(current_media, dict):
                media = dict(meta.get("media", {}))
                for key, value in current_media.items():
                    if value:
                        media[key] = value
                meta["media"] = media
            for key in ("last_trigger_unix_ms", "end_time", "trigger_count", "merged_triggers", "fields"):
                if key in current:
                    meta[key] = current[key]
        except (FileNotFoundError, OSError, ValueError, TypeError):
            pass
        with open(temporary, "w", encoding="utf-8") as stream:
            # manifest 同时用于板端人工排障。Python 更新投递状态后也必须保持可读格式，
            # 避免 C++ 初次写得整齐、服务一重试又被压成单行。
            json.dump(meta, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, final)

    def _profile(self, delivery: Dict[str, Any]) -> Dict[str, Any]:
        profile_id = str(delivery.get("profile_id", ""))
        if profile_id and profile_id not in self.profiles:
            raise ValueError(f"profile {profile_id} not found")
        profile = self.profiles.get(profile_id, {}) if profile_id else {}
        if profile_id and not isinstance(profile, dict):
            raise ValueError(f"profile {profile_id} is invalid")
        result = dict(profile) if isinstance(profile, dict) else {}
        profile_type = str(result.get("type", ""))
        target = str(delivery.get("target", ""))
        if profile_type and profile_type != target:
            raise ValueError(f"profile {profile_id} type is {profile_type}, expected {target}")
        return result

    @staticmethod
    def _lookup(meta: Dict[str, Any], source: str):
        if source.startswith("logic."):
            return meta.get("fields", {}).get(source[6:])
        if source.startswith("channel."):
            key = source[8:]
            return meta.get("channel_id") if key == "id" else meta.get("channel_parameters", {}).get(key)
        if source.startswith("event."):
            aliases = {
                "id": "event_id", "type": "alarm_type", "message": "message",
                "trigger_time": "trigger_unix_ms",
            }
            return meta.get(aliases.get(source[6:], source[6:]))
        return None

    @staticmethod
    def _set_path(root: Dict[str, Any], path: str, value: Any):
        parts = [part for part in path.split(".") if part]
        if not parts:
            raise ValueError("mapped JSON key is empty")
        current = root
        for part in parts[:-1]:
            child = current.get(part)
            if not isinstance(child, dict):
                child = {}
                current[part] = child
            current = child
        current[parts[-1]] = value

    @staticmethod
    def _coerce(value: Any, value_type: str) -> Any:
        if value is None or not value_type:
            return value
        if value_type == "string":
            return str(value)
        if value_type == "number":
            number = float(value)
            return int(number) if number.is_integer() else number
        if value_type == "boolean":
            if isinstance(value, bool):
                return value
            text = str(value).strip().lower()
            if text in ("true", "1", "yes", "on"):
                return True
            if text in ("false", "0", "no", "off"):
                return False
            raise ValueError(f"invalid boolean value: {value}")
        if value_type == "json":
            return json.loads(value) if isinstance(value, str) else value
        raise ValueError(f"unsupported input type: {value_type}")

    def _mapped_inputs(self, meta: Dict[str, Any], delivery: Dict[str, Any]) -> Dict[str, Any]:
        result: Dict[str, Any] = {}
        mappings = delivery.get("inputs", [])
        if not isinstance(mappings, list):
            raise ValueError("delivery inputs must be a list")
        for mapping in mappings:
            if not isinstance(mapping, dict):
                raise ValueError("delivery input mapping must be an object")
            key = str(mapping.get("key", "")).strip()
            source = str(mapping.get("source", "")).strip()
            # 画布中尚未填写完成的可选映射不应阻塞整个告警；必填映射仍严格报错。
            if not key:
                if mapping.get("required", False):
                    raise ValueError("required mapped JSON key is empty")
                continue
            value = mapping.get("value") if source == "constant" else self._lookup(meta, source)
            if value is None and mapping.get("required", False):
                raise ValueError(f"missing required input: {key} <- {source}")
            if value is not None:
                self._set_path(result, key, self._coerce(value, str(mapping.get("type", ""))))
        return result

    @staticmethod
    def _business_event(meta: Dict[str, Any]) -> Dict[str, Any]:
        """生成给 Dify 的纯业务事件，不暴露发件箱投递状态和策略快照。"""
        fields = meta.get("fields", {})
        source = fields.get("event_payload", {}) if isinstance(fields, dict) else {}
        payload = dict(source) if isinstance(source, dict) else {}
        business = {
            "schema_version": 2,
            "event_id": meta.get("event_id", ""),
            "channel_id": meta.get("channel_id"),
            "trigger_unix_ms": meta.get("trigger_unix_ms"),
            "snap_time": meta.get("snap_time", ""),
            "end_time": meta.get("end_time") or meta.get("snap_time", ""),
        }
        business.update(payload)
        return business

    @staticmethod
    def _lookup_business(root: Dict[str, Any], path: str):
        current: Any = root
        for part in (part for part in path.split(".") if part):
            if not isinstance(current, dict) or part not in current:
                return _MISSING
            current = current[part]
        return current

    def _mapped_business_event(self, meta: Dict[str, Any], delivery: Dict[str, Any]) -> Dict[str, Any]:
        """按 Web 保存的字段清单组装最终 event_json；旧配置继续全量发送。"""
        complete = self._business_event(meta)
        if "event_fields" not in delivery:
            return complete

        mappings = delivery.get("event_fields")
        if not isinstance(mappings, list):
            raise ValueError("delivery event_fields must be a list")

        result: Dict[str, Any] = {}
        for mapping in mappings:
            if not isinstance(mapping, dict):
                raise ValueError("event field mapping must be an object")
            source = str(mapping.get("source", "")).strip()
            target = str(mapping.get("target", "")).strip()
            required = bool(mapping.get("required", False))
            if not source or not target:
                if required:
                    raise ValueError("required event field source/target is empty")
                continue
            value = self._lookup_business(complete, source)
            if value is _MISSING:
                if required:
                    raise ValueError(f"missing required event field: {source}")
                continue
            value = self._coerce(value, str(mapping.get("type", "")))
            self._set_path(result, target, value)
        return result

    @staticmethod
    def _file_b64(path: str) -> str:
        with open(path, "rb") as stream:
            return base64.b64encode(stream.read()).decode()

    @staticmethod
    def _media_ready(event_dir: str, meta: Dict[str, Any], delivery: Dict[str, Any]) -> bool:
        media_type = str(delivery.get("media", "image"))
        if media_type == "json":
            return True
        target = str(delivery.get("target", ""))
        media = meta.get("media", {})
        if not isinstance(media, dict):
            return False
        if media_type == "video":
            filename = str(media.get("video", ""))
            return bool(filename) and os.path.isfile(os.path.join(event_dir, filename))
        snapshot_name = str(media.get("snapshot", ""))
        if not snapshot_name or not os.path.isfile(os.path.join(event_dir, snapshot_name)):
            return False
        if target == "server":
            raw_name = str(media.get("raw", ""))
            return bool(raw_name) and os.path.isfile(os.path.join(event_dir, raw_name))
        return True

    def _send_server_image(self, event_dir: str, meta: Dict[str, Any], delivery: Dict[str, Any]) -> bool:
        media = meta.get("media", {})
        snapshot = os.path.join(event_dir, str(media.get("snapshot", "")))
        raw_name = str(media.get("raw", ""))
        raw = os.path.join(event_dir, raw_name) if raw_name else snapshot
        if not os.path.isfile(snapshot) or not os.path.isfile(raw):
            raise MediaNotReadyError("image not ready")
        profile = self._profile(delivery)
        profile_id = str(delivery.get("profile_id", ""))
        url = str(profile.get("url", "")) if profile_id else self.server_url
        if not url:
            raise ValueError(f"server url missing for profile {profile_id}" if profile_id else "server url missing")
        payload = {
            "source": str(delivery.get("server_source", "JNU")),
            "eventType": str(delivery.get("server_event_type", "4005")),
            "detResult": {},
            "snapTime": meta.get("snap_time", ""),
            "endTime": meta.get("end_time") or meta.get("snap_time", ""),
            "base64Data": self._file_b64(snapshot),
            "base64DataRaw": self._file_b64(raw),
            "invadeFlag": 1,
            "eventId": meta.get("event_id", ""),
        }
        headers = {"X-Idempotency-Key": f"{meta.get('event_id', '')}:{delivery.get('id', '')}"}
        timeout = int(profile.get("timeout") or self.server_timeout)
        response = requests.post(url, json=payload, headers=headers, timeout=timeout)
        return response.status_code == 200

    def _send_dify(self, event_dir: str, meta: Dict[str, Any], delivery: Dict[str, Any]) -> bool:
        media_type = str(delivery.get("media", "image"))
        profile = self._profile(delivery)
        inputs = self._mapped_inputs(meta, delivery)
        event_variable = str(delivery.get("event_variable", "")).strip()
        if event_variable:
            inputs[event_variable] = json.dumps(
                self._mapped_business_event(meta, delivery),
                ensure_ascii=False,
                separators=(",", ":"),
            )
        data: Dict[str, Any] = {
            "event_id": meta.get("event_id", ""),
            "file_type": media_type,
            "input_variable": delivery.get("file_variable", "video" if media_type == "video" else "image"),
            "file_input_mode": delivery.get("file_input_mode", "single"),
            "inputs": inputs,
            "dify_api_url": profile.get("api_url", ""),
            "dify_api_key": profile.get("api_key", ""),
            "user": f"rk3588-ch{meta.get('channel_id', 0)}",
        }
        prompt = str(delivery.get("prompt", "")).strip()
        if prompt:
            data["prompt"] = prompt

        if media_type == "json":
            return self.dify.run_workflow(data)

        media = meta.get("media", {})
        filename = str(media.get("video" if media_type == "video" else "snapshot", ""))
        path = os.path.join(event_dir, filename) if filename else ""
        if not path or not os.path.isfile(path):
            raise MediaNotReadyError(f"{media_type} not ready")
        if media_type == "video":
            data["file_path"] = path
        else:
            data["base64Data"] = self._file_b64(path)
        return self.dify.upload(data)

    def _process(self, event_dir: str) -> bool:
        meta = self._read(event_dir)
        merge_window = float(meta.get("policy_snapshot", {}).get("merge_window_sec", 5.0))
        last_trigger = float(meta.get("last_trigger_unix_ms") or meta.get("trigger_unix_ms") or 0)
        deliveries = meta.get("deliveries", [])
        if not isinstance(deliveries, list) or not deliveries:
            return False

        changed = False
        for delivery in deliveries:
            if delivery.get("status") in ("delivered", "invalid"):
                continue
            now_ms = time.time() * 1000.0
            if delivery.get("status") == "retry" \
                    and now_ms < float(delivery.get("next_retry_unix_ms") or 0):
                continue
            media = str(delivery.get("media", "image"))
            # 图片不应用报警合并窗口；视频仍需等待合并窗口及录像文件完成。
            if media == "video" and last_trigger \
                    and time.time() * 1000.0 - last_trigger < merge_window * 1000.0:
                self._media_pending = True
                continue
            if not self._media_ready(event_dir, meta, delivery):
                self._media_pending = True
                continue
            delivery["status"] = "uploading"
            delivery["attempts"] = int(delivery.get("attempts", 0)) + 1
            self._write(event_dir, meta)
            try:
                target, media = delivery.get("target"), delivery.get("media")
                if target == "server" and media == "image":
                    ok = self._send_server_image(event_dir, meta, delivery)
                elif target == "dify" and media in ("image", "video", "json"):
                    ok = self._send_dify(event_dir, meta, delivery)
                else:
                    raise ValueError(f"unsupported delivery: {media}->{target}")
                delivery["status"] = "delivered" if ok else "retry"
                delivery["last_error"] = "" if ok else "remote rejected request"
                if ok:
                    delivery.pop("next_retry_unix_ms", None)
                else:
                    delivery["next_retry_unix_ms"] = time.time() * 1000.0 + self.RETRY_WAIT * 1000.0
            except MediaNotReadyError as exc:
                delivery["status"], delivery["last_error"] = "retry", str(exc)
                delivery.pop("next_retry_unix_ms", None)
                self._media_pending = True
            except ValueError as exc:
                delivery["status"], delivery["last_error"] = "invalid", str(exc)
                delivery.pop("next_retry_unix_ms", None)
            except Exception as exc:
                delivery["status"], delivery["last_error"] = "retry", str(exc)
                delivery["next_retry_unix_ms"] = time.time() * 1000.0 + self.RETRY_WAIT * 1000.0
            changed = True
            self._write(event_dir, meta)

        if all(item.get("status") == "delivered" for item in deliveries):
            event_id = meta.get("event_id", os.path.basename(event_dir))
            shutil.rmtree(event_dir)
            print(f"[EventOutbox] delivered and removed {event_id}")
            return True
        if changed:
            meta["state"] = "partial"
            self._write(event_dir, meta)
        return False

    def run(self, shutdown: Event):
        print(f"[EventOutbox] started, store={self.store_dir}")
        while not shutdown.is_set():
            self._media_pending = False
            paths = self._event_dirs()
            if not paths:
                shutdown.wait(self.SCAN_IDLE)
                continue
            sent = False
            for path in paths:
                if shutdown.is_set():
                    break
                try:
                    sent = self._process(path) or sent
                except FileNotFoundError:
                    continue
                except Exception as exc:
                    print(f"[EventOutbox] process failed {path}: {exc}")
            if not sent:
                shutdown.wait(self.MEDIA_WAIT if self._media_pending else self.RETRY_WAIT)
        print("[EventOutbox] stopped")
