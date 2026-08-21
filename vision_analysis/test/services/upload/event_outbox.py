"""持久化事件发件箱消费者。只负责状态机、重试和适配器分派。"""

import json
import os
import re
import shutil
import time
from pathlib import Path
from threading import Event
from typing import Any, Dict

import yaml

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
    MAX_RETRY_WAIT = 300.0
    RETRY_REQUEST = "delivery_retry.request.json"

    def __init__(
        self,
        store_dir: str,
        connections_path: Path,
        templates_dir: Path,
        custom_contracts_dir: Path,
        revisions_dir: Path,
        history_dir: Path,
    ):
        self.store_dir = store_dir
        self.connections_path = connections_path
        self.templates_dir = templates_dir
        self.custom_contracts_dir = custom_contracts_dir
        self.revisions_dir = revisions_dir
        self.history_dir = history_dir
        self.history_dir.mkdir(parents=True, exist_ok=True)
        self.connections: Dict[str, Dict[str, Any]] = {}
        self.contract_revisions: Dict[str, Dict[str, Any]] = {}
        self._media_pending = False

    def _reload_configuration(self) -> None:
        if self.connections_path.is_file():
            with self.connections_path.open("r", encoding="utf-8") as stream:
                document = yaml.safe_load(stream) or {}
        else:
            document = {}
        connections = document.get("connections", {})
        if not isinstance(connections, dict):
            raise ValueError("connections.yaml connections must be an object")
        _active, revisions = load_contracts(
            self.templates_dir, self.custom_contracts_dir, self.revisions_dir,
        )
        self.connections = connections
        self.contract_revisions = revisions

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
            "schema_version": 3,
            "deliveries": meta.get("deliveries", []),
        }
        with open(temporary, "w", encoding="utf-8") as stream:
            json.dump(document, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, final)

    def _write_history(
        self, meta: Dict[str, Any], delivery: Dict[str, Any], response: Any = None,
    ) -> None:
        event = meta.get("event", {})
        source = meta.get("source", {})
        event_id = str(event.get("id", "unknown"))
        delivery_id = str(delivery.get("id", "delivery"))
        updated_unix_ms = int(time.time() * 1000)
        document = {
            "event_id": event_id,
            "event_type": event.get("type", ""),
            "snap_time": event.get("snap_time", ""),
            "channel_id": source.get("channel_id"),
            "connection_id": delivery.get("connection_id", ""),
            "contract_id": delivery.get("contract_id", ""),
            "contract_revision": delivery.get("contract_revision", ""),
            "status": delivery.get("status", ""),
            "attempts": delivery.get("attempts", 0),
            "http_status": delivery.get("last_http_status", 0),
            "detail": delivery.get("last_error", ""),
            "response": response,
            "updated_unix_ms": updated_unix_ms,
        }
        safe_event_id = re.sub(r"[^A-Za-z0-9._-]", "_", event_id)
        safe_delivery_id = re.sub(r"[^A-Za-z0-9._-]", "_", delivery_id)
        attempt = max(0, int(delivery.get("attempts", 0)))
        destination = self.history_dir / (
            f"{safe_event_id}.{safe_delivery_id}.{attempt}.{time.time_ns()}.json"
        )
        temporary = destination.with_suffix(destination.suffix + ".tmp")
        with temporary.open("w", encoding="utf-8") as stream:
            json.dump(document, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, destination)
        limit = max(1, int(os.environ.get("DELIVERY_HISTORY_MAX_RECORDS", "1000")))
        files = sorted(
            self.history_dir.glob("*.json"),
            key=lambda path: path.stat().st_mtime,
            reverse=True,
        )
        for expired in files[limit:]:
            try:
                expired.unlink()
            except FileNotFoundError:
                pass

    def _connection(self, delivery: Dict[str, Any]) -> Dict[str, Any]:
        connection_id = str(delivery.get("connection_id", "")).strip()
        adapter_id = str(delivery.get("adapter", "")).strip()
        if not connection_id:
            raise ValueError("delivery connection_id is empty")
        connection = self.connections.get(connection_id)
        if not isinstance(connection, dict):
            raise ValueError(f"connection {connection_id} not found")
        if str(connection.get("adapter", "")).strip() != adapter_id:
            raise ValueError(f"connection {connection_id} does not use adapter {adapter_id}")
        return connection

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
                delivery["retry_streak"] = 0
                delivery.pop("next_retry_unix_ms", None)
        self._write(event_dir, meta)
        try:
            os.unlink(request)
        except FileNotFoundError:
            pass

    @classmethod
    def _retry_delay(cls, retry_streak: int) -> float:
        exponent = min(max(int(retry_streak) - 1, 0), 20)
        return min(cls.MAX_RETRY_WAIT, cls.RETRY_WAIT * (2 ** exponent))

    @classmethod
    def _schedule_retry(
        cls,
        delivery: Dict[str, Any],
        now_ms: float,
        detail: str,
        status_code: int = None,
    ) -> None:
        retry_streak = int(delivery.get("retry_streak", 0)) + 1
        delivery["status"] = "retry"
        delivery["retry_streak"] = retry_streak
        delivery["last_error"] = detail
        if status_code is not None:
            delivery["last_http_status"] = status_code
        delivery["next_retry_unix_ms"] = (
            now_ms + cls._retry_delay(retry_streak) * 1000.0
        )

    def _process(self, event_dir: str) -> bool:
        self._reload_configuration()
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
                effective_delivery = apply_contract(delivery, self.contract_revisions)
            except ValueError as exc:
                delivery["status"] = "invalid"
                delivery["last_error"] = str(exc)
                delivery.pop("next_retry_unix_ms", None)
                self._write(event_dir, meta)
                self._write_history(meta, delivery)
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
                self._write_history(meta, delivery)
                continue

            attempts = int(delivery.get("attempts", 0)) + 1
            delivery["status"] = "uploading"
            delivery["attempts"] = attempts
            self._write(event_dir, meta)
            try:
                adapter_id = str(effective_delivery.get("adapter", "")).strip()
                adapter = create_adapter(adapter_id, self._connection(effective_delivery))
                result = adapter.send(event_dir, meta, effective_delivery)
                if not result.ok and getattr(result, "terminal", False):
                    delivery["status"] = "failed"
                    delivery["last_error"] = result.detail
                    delivery["last_http_status"] = result.status_code
                    delivery.pop("retry_streak", None)
                    delivery.pop("next_retry_unix_ms", None)
                elif result.ok:
                    delivery["status"] = "delivered"
                    delivery["last_error"] = ""
                    delivery["last_http_status"] = result.status_code
                    delivery.pop("retry_streak", None)
                    delivery.pop("next_retry_unix_ms", None)
                else:
                    self._schedule_retry(
                        delivery, now_ms, result.detail, result.status_code,
                    )
                self._write_history(meta, delivery, result.response)
            except MediaNotReadyError as exc:
                delivery["status"] = "retry"
                delivery["last_error"] = str(exc)
                delivery.pop("next_retry_unix_ms", None)
                self._media_pending = True
            except ValueError as exc:
                delivery["status"] = "invalid"
                delivery["last_error"] = str(exc)
                delivery.pop("next_retry_unix_ms", None)
                self._write_history(meta, delivery)
            except Exception as exc:
                self._schedule_retry(delivery, now_ms, str(exc))
                self._write_history(meta, delivery)
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
        print(f"[EventOutbox] started, store={self.store_dir}, templates={self.templates_dir}")
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
