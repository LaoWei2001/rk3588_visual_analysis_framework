"""持久化事件发件箱消费者。只负责状态机、重试和适配器分派。"""

import json
import os
import shutil
import time
from threading import Event
from typing import Any, Dict

from adapters import create_adapter
from contracts import apply_contract, load_contracts


class MediaNotReadyError(RuntimeError):
    pass


class MediaGenerationFailedError(RuntimeError):
    pass


class EventOutboxForwarder:
    SCAN_IDLE = 0.5
    MEDIA_WAIT = 0.2
    RETRY_WAIT = 10.0
    MAX_RETRIES = 12
    RETRY_REQUEST = "delivery_retry.request.json"

    def __init__(
        self,
        config: Dict[str, Any],
        store_dir: str,
        contracts: Dict[str, Dict[str, Any]] = None,
    ):
        self.store_dir = store_dir
        profiles = config.get("profiles", {})
        self.profiles = profiles if isinstance(profiles, dict) else {}
        self.contracts = contracts if contracts is not None else load_contracts()
        self._media_pending = False

    def _event_dirs(self):
        try:
            names = os.listdir(self.store_dir)
        except FileNotFoundError:
            return []
        paths = []
        for name in names:
            path = os.path.join(self.store_dir, name)
            event_json = os.path.join(path, "event.json")
            try:
                if os.path.isdir(path) and os.path.isfile(event_json):
                    paths.append((os.path.getmtime(event_json), path))
            except FileNotFoundError:
                continue
        paths.sort(key=lambda item: item[0])
        return [path for _, path in paths]

    @staticmethod
    def _read(path: str) -> Dict[str, Any]:
        with open(os.path.join(path, "event.json"), "r", encoding="utf-8") as stream:
            event_doc = json.load(stream)
        with open(os.path.join(path, "media_state.json"), "r", encoding="utf-8") as stream:
            media_doc = json.load(stream)
        with open(os.path.join(path, "delivery_state.json"), "r", encoding="utf-8") as stream:
            delivery_doc = json.load(stream)

        event = event_doc.get("event", {})
        source = event_doc.get("source", {})
        data = event_doc.get("data", {})
        media_entries = media_doc.get("media", {})
        deliveries = delivery_doc.get("deliveries", [])
        if not all(isinstance(item, dict) for item in (
            event, source, data, media_doc, media_entries, delivery_doc,
        )) or not isinstance(deliveries, list):
            raise ValueError("invalid event document")

        media: Dict[str, str] = {}
        for kind, entry in media_entries.items():
            if not isinstance(entry, dict) or not isinstance(entry.get("files", {}), dict):
                raise ValueError(f"invalid {kind} media state")
            for variant, filename in entry["files"].items():
                media[str(variant)] = os.path.join(path, str(filename)) if filename else ""

        return {
            "schema_version": event_doc.get("schema_version", 3),
            "event": event,
            "source": source,
            "fields": data.get("fields", {}),
            "media": media,
            "media_state": media_entries,
            "overall_media_status": media_doc.get("status", "ready"),
            "policy": event_doc.get("policy_snapshot", {}),
            "deliveries": deliveries,
        }

    @staticmethod
    def _write(path: str, meta: Dict[str, Any]):
        final = os.path.join(path, "delivery_state.json")
        temporary = final + ".upload.tmp"
        document = {
            "schema_version": 2,
            "deliveries": meta.get("deliveries", []),
        }
        with open(temporary, "w", encoding="utf-8") as stream:
            json.dump(document, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, final)

    def _profile(self, delivery: Dict[str, Any]) -> Dict[str, Any]:
        profile_id = str(delivery.get("profile_id", "")).strip()
        adapter_id = str(delivery.get("adapter", "")).strip()
        if not profile_id:
            raise ValueError("delivery profile_id is empty")
        profile = self.profiles.get(profile_id)
        if not isinstance(profile, dict):
            raise ValueError(f"profile {profile_id} not found")
        if str(profile.get("adapter", "")).strip() != adapter_id:
            raise ValueError(f"profile {profile_id} does not use adapter {adapter_id}")
        return profile

    @staticmethod
    def _media_ready(meta: Dict[str, Any], delivery: Dict[str, Any]) -> bool:
        required = delivery.get("media", [])
        if not isinstance(required, list):
            raise ValueError("delivery media must be an array")
        for variant in required:
            variant = str(variant)
            state_kind = "video" if variant == "video" else "image"
            state = meta.get("media_state", {}).get(state_kind)
            if not isinstance(state, dict):
                raise MediaGenerationFailedError(f"{state_kind} state is missing")
            status = str(state.get("status", ""))
            if status == "failed":
                error = str(state.get("error", "")).strip()
                raise MediaGenerationFailedError(
                    f"{state_kind} generation failed: {error or 'unknown error'}"
                )
            if status in ("requested", "generating"):
                return False
            if status != "ready":
                raise MediaGenerationFailedError(
                    f"{state_kind} has invalid media status: {status}"
                )
            path = str(meta.get("media", {}).get(variant, ""))
            if not path or not os.path.isfile(path):
                raise MediaGenerationFailedError(
                    f"{variant} is ready but its file is missing"
                )
        return True

    def _reset_requested_retries(self, event_dir: str, meta: Dict[str, Any]) -> None:
        request = os.path.join(event_dir, self.RETRY_REQUEST)
        if not os.path.isfile(request):
            return
        for delivery in meta.get("deliveries", []):
            if delivery.get("status") != "delivered":
                delivery["status"] = "pending"
                delivery["last_error"] = ""
                delivery.pop("next_retry_unix_ms", None)
        self._write(event_dir, meta)
        try:
            os.unlink(request)
        except FileNotFoundError:
            pass

    def _process(self, event_dir: str) -> bool:
        meta = self._read(event_dir)
        self._reset_requested_retries(event_dir, meta)

        merge_window = float(meta.get("policy", {}).get("merge_window_sec", 5.0))
        event = meta.get("event", {})
        last_trigger = float(
            event.get("last_trigger_unix_ms") or event.get("trigger_unix_ms") or 0
        )
        deliveries = meta.get("deliveries", [])
        if not deliveries:
            return False

        for delivery in deliveries:
            if not isinstance(delivery, dict):
                continue
            if delivery.get("status") in ("delivered", "invalid", "failed"):
                continue
            try:
                effective_delivery = apply_contract(delivery, self.contracts)
            except ValueError as exc:
                delivery["status"] = "invalid"
                delivery["last_error"] = str(exc)
                delivery.pop("next_retry_unix_ms", None)
                self._write(event_dir, meta)
                continue
            now_ms = time.time() * 1000.0
            if delivery.get("status") == "retry" and now_ms < float(
                delivery.get("next_retry_unix_ms") or 0
            ):
                continue
            media = effective_delivery.get("media", [])
            if isinstance(media, list) and "video" in media and last_trigger and (
                now_ms - last_trigger < merge_window * 1000.0
            ):
                self._media_pending = True
                continue
            try:
                if not self._media_ready(meta, effective_delivery):
                    self._media_pending = True
                    continue
            except MediaGenerationFailedError as exc:
                delivery["status"] = "failed"
                delivery["last_error"] = str(exc)
                delivery.pop("next_retry_unix_ms", None)
                self._write(event_dir, meta)
                continue

            attempts = int(delivery.get("attempts", 0)) + 1
            delivery["status"] = "uploading"
            delivery["attempts"] = attempts
            self._write(event_dir, meta)
            try:
                adapter_id = str(effective_delivery.get("adapter", "")).strip()
                adapter = create_adapter(adapter_id, self._profile(effective_delivery))
                result = adapter.send(event_dir, meta, effective_delivery)
                if not result.ok and getattr(result, "terminal", False):
                    delivery["status"] = "failed"
                    delivery["last_error"] = result.detail
                    delivery["last_http_status"] = result.status_code
                    delivery.pop("next_retry_unix_ms", None)
                else:
                    delivery["status"] = "delivered" if result.ok else "retry"
                    delivery["last_error"] = "" if result.ok else result.detail
                    delivery["last_http_status"] = result.status_code
                    if result.ok:
                        delivery.pop("next_retry_unix_ms", None)
                    elif attempts >= self.MAX_RETRIES:
                        delivery["status"] = "failed"
                        delivery["last_error"] = f"max retries ({self.MAX_RETRIES}) exceeded: {result.detail}"
                        delivery.pop("next_retry_unix_ms", None)
                    else:
                        delivery["next_retry_unix_ms"] = now_ms + self.RETRY_WAIT * 1000.0
            except MediaNotReadyError as exc:
                delivery["status"] = "retry"
                delivery["last_error"] = str(exc)
                delivery.pop("next_retry_unix_ms", None)
                self._media_pending = True
            except ValueError as exc:
                delivery["status"] = "invalid"
                delivery["last_error"] = str(exc)
                delivery.pop("next_retry_unix_ms", None)
            except Exception as exc:
                if attempts >= self.MAX_RETRIES:
                    delivery["status"] = "failed"
                    delivery["last_error"] = f"max retries ({self.MAX_RETRIES}) exceeded: {exc}"
                    delivery.pop("next_retry_unix_ms", None)
                else:
                    delivery["status"] = "retry"
                    delivery["last_error"] = str(exc)
                    delivery["next_retry_unix_ms"] = now_ms + self.RETRY_WAIT * 1000.0
            self._write(event_dir, meta)

        if all(
            isinstance(item, dict) and item.get("status") == "delivered"
            for item in deliveries
        ):
            event_id = event.get("id", os.path.basename(event_dir))
            shutil.rmtree(event_dir)
            print(f"[EventOutbox] delivered and removed {event_id}")
            return True
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
