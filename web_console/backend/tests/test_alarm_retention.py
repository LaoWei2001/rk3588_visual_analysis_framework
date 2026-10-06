import json
import time
from types import SimpleNamespace

import pytest
from services import storage_manager


def event(store, name, deliveries, media_status="ready"):
    path = store / name
    path.mkdir()
    (path / "event.json").write_text(json.dumps({"event": {"created_unix_sec": time.time() - 40 * 86400}}))
    (path / "media_state.json").write_text(json.dumps({"media": {"image": {"status": media_status}}}))
    (path / "delivery_state.json").write_text(json.dumps({"deliveries": deliveries}))
    return path


@pytest.mark.parametrize("status", ["pending", "retry", "uploading", "invalid", "failed", "unknown"])
def test_cleanup_preserves_unsent_alarm_under_age_size_and_disk_pressure(tmp_path, monkeypatch, status):
    protected = event(tmp_path, "unsent", [{"status": status}])
    completed = event(tmp_path, "sent", [{"status": "delivered"}])
    monkeypatch.setattr(storage_manager, "_event_stores", lambda: [tmp_path])
    monkeypatch.setattr(storage_manager, "read_settings", lambda: {
        "retention_days": 1, "max_event_store_gb": 0, "min_free_gb": 1,
    })
    monkeypatch.setattr(storage_manager.shutil, "disk_usage", lambda _: SimpleNamespace(free=0))
    result = storage_manager.cleanup_now()
    assert protected.exists()
    assert not completed.exists()
    assert result["deleted_count"] == 1
    assert result["skipped_active"] == 1


@pytest.mark.parametrize("deliveries", [[], None, {}, [None], [{}], [{"status": "delivered"}, {"status": "retry"}]])
def test_malformed_and_partially_sent_records_are_protected(tmp_path, deliveries):
    record = event(tmp_path, "alarm", deliveries)
    assert storage_manager._event_info(tmp_path, record)["active"]


def test_media_generation_still_protects_completed_delivery(tmp_path):
    record = event(tmp_path, "alarm", [{"status": "delivered"}], "generating")
    assert storage_manager._event_info(tmp_path, record)["active"]
