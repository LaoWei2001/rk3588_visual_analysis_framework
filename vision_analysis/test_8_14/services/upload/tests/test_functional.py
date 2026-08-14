"""功能测试：合约加载、字段映射、事件读取、媒体就绪检查、适配器预览。"""

import json
import os
import sys
import tempfile
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from contracts import ALLOWED_MEDIA, CONTRACT_KEYS, apply_contract, load_contracts
from adapters.mapping import (
    MISSING, coerce, lookup, mapped_values, response_path, set_path, transform_value,
)
from adapters.base import DeliveryResult, is_retryable_http_status
from event_outbox import EventOutboxForwarder, MediaGenerationFailedError


# ── contracts ────────────────────────────────────────────────────────────────

class TestLoadContracts:
    def test_loads_valid_contracts(self):
        contracts = load_contracts()
        assert isinstance(contracts, dict)
        assert "dify_event_data" in contracts
        assert "dify_event_video" in contracts
        for cid, contract in contracts.items():
            assert isinstance(contract, dict)
            assert contract.get("id") == cid
            assert contract["adapter"] in {"dify_workflow", "http_json"}
            assert isinstance(contract["media"], list)
            assert isinstance(contract["mapping"], list)

    def test_rejects_nonexistent_directory(self):
        with pytest.raises(ValueError, match="not found"):
            load_contracts(Path("/nonexistent_contracts_dir_xyz"))

    def test_rejects_duplicate_ids(self, tmp_path):
        contract = {"id": "dup", "adapter": "dify_workflow", "media": [], "mapping": []}
        (tmp_path / "a.json").write_text(json.dumps(contract))
        (tmp_path / "b.json").write_text(json.dumps(contract))
        with pytest.raises(ValueError, match="duplicate"):
            load_contracts(tmp_path)

    def test_rejects_invalid_json(self, tmp_path):
        (tmp_path / "bad.json").write_text("not json")
        with pytest.raises(ValueError, match="invalid report contract"):
            load_contracts(tmp_path)

    def test_rejects_missing_id(self, tmp_path):
        (tmp_path / "no_id.json").write_text(json.dumps({"adapter": "dify_workflow", "media": [], "mapping": []}))
        with pytest.raises(ValueError, match="requires id"):
            load_contracts(tmp_path)

    def test_rejects_invalid_media(self, tmp_path):
        (tmp_path / "bad_media.json").write_text(json.dumps(
            {"id": "x", "adapter": "dify_workflow", "media": ["bogus"], "mapping": []}))
        with pytest.raises(ValueError, match="invalid media"):
            load_contracts(tmp_path)

    def test_rejects_non_array_mapping(self, tmp_path):
        (tmp_path / "bad_map.json").write_text(json.dumps(
            {"id": "x", "adapter": "dify_workflow", "media": [], "mapping": "not_array"}))
        with pytest.raises(ValueError, match="mapping must be an array"):
            load_contracts(tmp_path)


class TestApplyContract:
    def test_applies_contract_keys(self):
        contracts = load_contracts()
        delivery = {
            "profile_id": "test",
            "contract_id": "dify_event_data",
            "media": [],
        }
        effective = apply_contract(delivery, contracts)
        for key in ("adapter", "media", "mapping"):
            assert key in effective
        assert effective["adapter"] == "dify_workflow"

    def test_rejects_empty_contract_id(self):
        with pytest.raises(ValueError, match="contract_id is empty"):
            apply_contract({"contract_id": ""}, {})

    def test_rejects_unknown_contract(self):
        with pytest.raises(ValueError, match="not found"):
            apply_contract({"contract_id": "no_such_contract"}, {})

    def test_rejects_media_mismatch(self):
        contracts = load_contracts()
        delivery = {
            "contract_id": "dify_event_video",
            "media": ["annotated_image"],  # contract requires ["video"]
        }
        with pytest.raises(ValueError, match="media snapshot does not match"):
            apply_contract(delivery, contracts)


# ── mapping ──────────────────────────────────────────────────────────────────

class TestLookup:
    def test_lookup_top_level(self):
        doc = {"a": 1, "b": {"c": 2}}
        assert lookup(doc, "a") == 1
        assert lookup(doc, "b") == {"c": 2}

    def test_lookup_nested(self):
        doc = {"a": {"b": {"c": 3}}}
        assert lookup(doc, "a.b.c") == 3

    def test_lookup_missing(self):
        doc = {"a": 1}
        assert lookup(doc, "x") is MISSING
        assert lookup(doc, "a.b") is MISSING
        assert lookup(doc, "constant") is MISSING

    def test_lookup_non_dict_intermediate(self):
        doc = {"a": 1}
        assert lookup(doc, "a.b.c") is MISSING


class TestSetPath:
    def test_set_simple(self):
        root = {}
        set_path(root, "key", "val")
        assert root == {"key": "val"}

    def test_set_nested(self):
        root = {}
        set_path(root, "a.b.c", 42)
        assert root == {"a": {"b": {"c": 42}}}

    def test_set_nested_existing(self):
        root = {"a": {"x": 1}}
        set_path(root, "a.y", 2)
        assert root == {"a": {"x": 1, "y": 2}}

    def test_set_empty_path_raises(self):
        with pytest.raises(ValueError, match="empty"):
            set_path({}, "", "x")


class TestCoerce:
    def test_string(self):
        assert coerce(123, "string") == "123"

    def test_number_int(self):
        assert coerce("42", "number") == 42
        assert coerce(3.0, "number") == 3

    def test_number_float(self):
        assert coerce("3.14", "number") == 3.14

    def test_boolean(self):
        assert coerce("true", "boolean") is True
        assert coerce("false", "boolean") is False
        assert coerce("1", "boolean") is True
        assert coerce("0", "boolean") is False

    def test_json(self):
        assert coerce('{"a":1}', "json") == {"a": 1}

    def test_none_passthrough(self):
        assert coerce(None, "string") is None
        assert coerce(None, "") is None

    def test_invalid_boolean(self):
        with pytest.raises(ValueError, match="invalid boolean"):
            coerce("maybe", "boolean")

    def test_unsupported_type(self):
        with pytest.raises(ValueError, match="unsupported mapping type"):
            coerce("x", "unknown_type")


class TestTransformValue:
    def test_no_transform(self):
        assert transform_value("hello", "") == "hello"

    def test_json_string(self):
        assert transform_value({"a": 1}, "json_string") == '{"a":1}'

    def test_file_passthrough(self, tmp_path):
        f = tmp_path / "test.txt"
        f.write_text("content")
        assert transform_value(str(f), "file") == str(f)

    def test_unsupported_transform(self):
        with pytest.raises(ValueError, match="unsupported mapping transform"):
            transform_value("x", "unknown")


class TestMappedValues:
    def test_simple_field_mapping(self):
        event = {"fields": {"score": 0.95}}
        mappings = [{"source": "fields.score", "target": "confidence"}]
        values, files = mapped_values(event, mappings)
        assert values == {"confidence": 0.95}
        assert files == {}

    def test_constant_mapping(self):
        event = {}
        mappings = [{"source": "constant", "target": "type", "value": "alarm"}]
        values, files = mapped_values(event, mappings)
        assert values == {"type": "alarm"}

    def test_constant_with_type_coercion(self):
        event = {}
        mappings = [{"source": "constant", "target": "count", "value": "42", "type": "number"}]
        values, files = mapped_values(event, mappings)
        assert values == {"count": 42}

    def test_nested_target(self):
        event = {"event": {"id": "evt-1"}}
        mappings = [{"source": "event.id", "target": "meta.event_id"}]
        values, files = mapped_values(event, mappings)
        assert values == {"meta": {"event_id": "evt-1"}}

    def test_required_missing_raises(self):
        event = {}
        mappings = [{"source": "event.id", "target": "id", "required": True}]
        with pytest.raises(ValueError, match="required mapping source is missing"):
            mapped_values(event, mappings)

    def test_optional_missing_skipped(self):
        event = {}
        mappings = [{"source": "event.id", "target": "id", "required": False}]
        values, files = mapped_values(event, mappings)
        assert values == {}

    def test_empty_source_target_skipped_when_not_required(self):
        event = {}
        mappings = [{"source": "", "target": "", "required": False}]
        values, files = mapped_values(event, mappings)
        assert values == {}

    def test_empty_source_target_raises_when_required(self):
        event = {}
        mappings = [{"source": "", "target": "", "required": True}]
        with pytest.raises(ValueError, match="required mapping source/target is empty"):
            mapped_values(event, mappings)

    def test_json_string_transform(self):
        event = {"event": {"data": {"x": 1}}}
        mappings = [{"source": "event.data", "target": "payload", "transform": "json_string"}]
        values, files = mapped_values(event, mappings)
        assert values == {"payload": '{"x":1}'}


class TestResponsePath:
    def test_simple_path(self):
        assert response_path({"code": 200}, "code") == 200

    def test_nested_path(self):
        assert response_path({"data": {"status": "ok"}}, "data.status") == "ok"

    def test_missing_path(self):
        assert response_path({"a": 1}, "b") is MISSING
        assert response_path({"a": 1}, "a.b") is MISSING

    def test_non_dict_intermediate(self):
        assert response_path({"a": 1}, "a.b.c") is MISSING


# ── event_outbox ─────────────────────────────────────────────────────────────

def _make_event_dir(base: str, event_data: dict = None, media_state: dict = None,
                    delivery_state: dict = None):
    """Helper: 在 *base* 下创建最小事件目录, 返回目录路径."""
    event_id = "ch0_20260731_test_000"
    event_dir = os.path.join(base, event_id)
    os.makedirs(event_dir, exist_ok=True)

    event = event_data or {
        "schema_version": 3,
        "event": {"id": event_id, "type": "test_event", "message": "test",
                  "trigger_unix_ms": 1700000000000, "snap_time": "2023-01-01 00:00:00"},
        "source": {"channel_id": 0},
        "data": {"fields": {"value": 42}},
        "policy_snapshot": {"enabled": True, "merge_window_sec": 5},
    }
    with open(os.path.join(event_dir, "event.json"), "w") as f:
        json.dump(event, f)

    media = media_state or {"status": "ready", "media": {
        "image": {"status": "ready", "files": {"annotated": "annotated.jpg", "raw": "raw.jpg"}},
    }}
    with open(os.path.join(event_dir, "media_state.json"), "w") as f:
        json.dump(media, f)

    deliveries = delivery_state or {"schema_version": 2, "deliveries": [{
        "id": "d1", "enabled": True, "profile_id": "test_p", "contract_id": "dify_event_data",
        "media": [], "status": "pending",
    }]}
    with open(os.path.join(event_dir, "delivery_state.json"), "w") as f:
        json.dump(deliveries, f)

    return event_dir


class TestEventRead:
    def test_reads_valid_event(self, tmp_path):
        event_dir = _make_event_dir(str(tmp_path))
        meta = EventOutboxForwarder._read(event_dir)
        assert meta["schema_version"] == 3
        assert meta["event"]["id"] == "ch0_20260731_test_000"
        assert meta["fields"] == {"value": 42}
        assert isinstance(meta["deliveries"], list)
        assert len(meta["deliveries"]) == 1

    def test_read_raises_on_missing_event_json(self, tmp_path):
        d = tmp_path / "empty"
        d.mkdir()
        with pytest.raises(FileNotFoundError):
            EventOutboxForwarder._read(str(d))


class TestMediaReady:
    def test_media_ready_no_media(self):
        meta = {"media_state": {}, "media": {}}
        assert EventOutboxForwarder._media_ready(meta, {"media": []}) is True

    def test_media_ready_image(self, tmp_path):
        event_dir = _make_event_dir(str(tmp_path), media_state={
            "status": "ready",
            "media": {"image": {"status": "ready", "files": {
                "annotated_image": "annotated.jpg", "raw_image": "raw.jpg",
            }}},
        })
        (Path(event_dir) / "annotated.jpg").write_text("fake-jpeg")
        (Path(event_dir) / "raw.jpg").write_text("fake-jpeg")
        meta = EventOutboxForwarder._read(event_dir)

        delivery = {"media": ["annotated_image", "raw_image"]}
        assert EventOutboxForwarder._media_ready(meta, delivery) is True

    def test_media_ready_missing_file(self, tmp_path):
        event_dir = _make_event_dir(str(tmp_path))
        meta = EventOutboxForwarder._read(event_dir)
        # 文件不存在
        delivery = {"media": ["annotated_image"]}
        with pytest.raises(MediaGenerationFailedError, match="file is missing"):
            EventOutboxForwarder._media_ready(meta, delivery)

    def test_media_ready_failed_status(self, tmp_path):
        event_dir = _make_event_dir(str(tmp_path), media_state={
            "status": "ready",
            "media": {"image": {"status": "failed", "error": "encode error", "files": {}}},
        })
        meta = EventOutboxForwarder._read(event_dir)
        delivery = {"media": ["annotated_image"]}
        with pytest.raises(MediaGenerationFailedError, match="generation failed"):
            EventOutboxForwarder._media_ready(meta, delivery)

    def test_media_ready_generating_returns_false(self, tmp_path):
        event_dir = _make_event_dir(str(tmp_path), media_state={
            "status": "ready",
            "media": {"image": {"status": "generating", "files": {}}},
        })
        meta = EventOutboxForwarder._read(event_dir)
        delivery = {"media": ["annotated_image"]}
        assert EventOutboxForwarder._media_ready(meta, delivery) is False

    def test_media_ready_requested_returns_false(self, tmp_path):
        event_dir = _make_event_dir(str(tmp_path), media_state={
            "status": "ready",
            "media": {"image": {"status": "requested", "files": {}}},
        })
        meta = EventOutboxForwarder._read(event_dir)
        delivery = {"media": ["annotated_image"]}
        assert EventOutboxForwarder._media_ready(meta, delivery) is False

    def test_media_ready_invalid_delivery_media(self):
        meta = {"media_state": {}, "media": {}}
        with pytest.raises(ValueError, match="must be an array"):
            EventOutboxForwarder._media_ready(meta, {"media": "not_array"})


class TestOutboxRetry:
    @staticmethod
    def _forwarder(store_dir):
        return EventOutboxForwarder(
            {"profiles": {"test_p": {"adapter": "http_json"}}},
            str(store_dir),
            contracts={
                "dify_event_data": {
                    "id": "dify_event_data",
                    "adapter": "http_json",
                    "media": [],
                    "mapping": [],
                },
            },
        )

    def test_retry_delay_uses_capped_exponential_backoff(self):
        assert EventOutboxForwarder._retry_delay(1) == 10
        assert EventOutboxForwarder._retry_delay(2) == 20
        assert EventOutboxForwarder._retry_delay(5) == 160
        assert EventOutboxForwarder._retry_delay(6) == 300
        assert EventOutboxForwarder._retry_delay(1000) == 300

    def test_network_exception_keeps_retrying_after_old_attempt_limit(self, tmp_path):
        event_dir = _make_event_dir(str(tmp_path), delivery_state={
            "schema_version": 2,
            "deliveries": [{
                "id": "d1", "enabled": True, "profile_id": "test_p",
                "contract_id": "dify_event_data", "media": [],
                "status": "pending", "attempts": 12,
            }],
        })
        adapter = MagicMock()
        adapter.send.side_effect = ConnectionError("network offline")
        forwarder = self._forwarder(tmp_path)

        with patch("event_outbox.create_adapter", return_value=adapter), \
                patch("event_outbox.time.time", return_value=1000.0):
            assert forwarder._process(event_dir) is False

        state = json.loads(Path(event_dir, "delivery_state.json").read_text())
        delivery = state["deliveries"][0]
        assert delivery["status"] == "retry"
        assert delivery["attempts"] == 13
        assert delivery["retry_streak"] == 1
        assert delivery["next_retry_unix_ms"] == 1010000.0
        assert delivery["last_error"] == "network offline"

    def test_legacy_max_retry_failure_is_resumed_automatically(self, tmp_path):
        event_dir = _make_event_dir(str(tmp_path), delivery_state={
            "schema_version": 2,
            "deliveries": [{
                "id": "d1", "enabled": True, "profile_id": "test_p",
                "contract_id": "dify_event_data", "media": [],
                "status": "failed", "attempts": 12,
                "last_error": "max retries (12) exceeded: network offline",
            }],
        })
        adapter = MagicMock()
        adapter.send.return_value = DeliveryResult(True, "delivered", 200)
        forwarder = self._forwarder(tmp_path)

        with patch("event_outbox.create_adapter", return_value=adapter):
            assert forwarder._process(event_dir) is True

        adapter.send.assert_called_once()
        assert not Path(event_dir).exists()

    def test_terminal_delivery_result_stays_failed(self, tmp_path):
        event_dir = _make_event_dir(str(tmp_path))
        adapter = MagicMock()
        adapter.send.return_value = DeliveryResult(
            False, "remote rejected request: HTTP 401", 401, terminal=True,
        )
        forwarder = self._forwarder(tmp_path)

        with patch("event_outbox.create_adapter", return_value=adapter):
            assert forwarder._process(event_dir) is False

        state = json.loads(Path(event_dir, "delivery_state.json").read_text())
        delivery = state["deliveries"][0]
        assert delivery["status"] == "failed"
        assert "next_retry_unix_ms" not in delivery


def test_retryable_http_status_values():
    assert is_retryable_http_status(408)
    assert is_retryable_http_status(425)
    assert is_retryable_http_status(429)
    assert is_retryable_http_status(500)
    assert not is_retryable_http_status(400)
    assert not is_retryable_http_status(401)


# ── dify adapter preview (no network) ────────────────────────────────────────

class TestDifyAdapterPreview:
    def test_preview_structure(self):
        from adapters.dify_workflow import DifyWorkflowAdapter
        from adapters.registry import create_adapter

        profile = {"api_url": "http://dify.example.com/v1", "api_key": "test-key", "timeout": 60}
        adapter = DifyWorkflowAdapter(profile)

        event = {
            "event": {"id": "evt-1", "type": "test"},
            "source": {"channel_id": 0},
        }
        delivery = {
            "profile_id": "test",
            "contract_id": "dify_event_data",
            "adapter": "dify_workflow",
            "media": [],
            "mapping": [
                {"source": "event", "target": "event_json", "transform": "json_string", "required": True},
            ],
        }
        preview = adapter.preview("", event, delivery)
        assert preview["adapter"] == "dify_workflow"
        assert preview["method"] == "POST"
        assert "upload_url" in preview
        assert "workflow_url" in preview
        assert "inputs" in preview
        assert "event_json" in preview["inputs"]

    def test_create_adapter_registry(self):
        from adapters.registry import create_adapter

        profile = {"api_url": "http://x", "api_key": "k"}
        adapter = create_adapter("dify_workflow", profile)
        assert adapter.adapter_id == "dify_workflow"

    def test_create_adapter_unknown_raises(self):
        from adapters.registry import create_adapter

        with pytest.raises(ValueError, match="unknown delivery adapter"):
            create_adapter("no_such", {})


# ── ALLOWED_MEDIA ────────────────────────────────────────────────────────────

def test_allowed_media_values():
    assert ALLOWED_MEDIA == {"annotated_image", "raw_image", "video"}
