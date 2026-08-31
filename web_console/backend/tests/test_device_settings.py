import json
import time
from pathlib import Path

import pytest
from routers import network_settings
from services import network_manager, storage_manager


def _event(store: Path, name: str, created: float, *, active: bool = False) -> Path:
    path = store / name
    path.mkdir(parents=True)
    (path / "event.json").write_text(json.dumps({
        "event": {"id": name, "created_unix_sec": created},
    }))
    (path / "media_state.json").write_text(json.dumps({
        "media": {"image": {"status": "generating" if active else "ready"}},
    }))
    (path / "delivery_state.json").write_text(json.dumps({
        "deliveries": [{"status": "pending"}],
    }))
    (path / "annotated.jpg").write_bytes(b"x" * 128)
    return path


def test_storage_cleanup_is_scoped_and_skips_active_events(tmp_path, monkeypatch):
    apps_root = tmp_path / "apps"
    store = apps_root / ".data" / "demo" / "event_store"
    old = _event(store, "old", time.time() - 10 * 86400)
    active = _event(store, "active", time.time() - 10 * 86400, active=True)
    unrelated = apps_root / "demo" / "assets" / "model.rknn"
    unrelated.parent.mkdir(parents=True)
    unrelated.write_bytes(b"model")

    monkeypatch.setattr(storage_manager, "APPS_ROOT", apps_root)
    monkeypatch.setattr(storage_manager, "SETTINGS_FILE", apps_root / ".storage.json")
    monkeypatch.setattr(storage_manager, "_last_cleanup", None)
    storage_manager.write_settings({
        "auto_cleanup": True,
        "retention_days": 1,
        "max_event_store_gb": 1,
        "min_free_gb": 0,
    })

    result = storage_manager.cleanup_now()

    assert result["deleted_count"] == 1
    assert not old.exists()
    assert active.exists()
    assert unrelated.read_bytes() == b"model"


def test_root_cleanup_only_removes_selected_allowlisted_targets(tmp_path, monkeypatch):
    root_home = tmp_path / "root"
    npm = root_home / ".npm"
    cache = root_home / ".cache"
    nvm = root_home / ".nvm"
    for path in (npm, cache, nvm):
        path.mkdir(parents=True)
        (path / "payload").write_bytes(b"x" * 32)

    monkeypatch.setattr(storage_manager, "ROOT_HOME", root_home)
    result = storage_manager.cleanup_root_targets(["npm", "cache"])

    assert result["deleted_count"] == 2
    assert result["freed_bytes"] == 64
    assert not npm.exists()
    assert not cache.exists()
    assert (nvm / "payload").read_bytes() == b"x" * 32


def test_root_cleanup_rejects_unknown_target_before_deleting(tmp_path, monkeypatch):
    root_home = tmp_path / "root"
    npm = root_home / ".npm"
    npm.mkdir(parents=True)
    (npm / "payload").write_bytes(b"keep")
    monkeypatch.setattr(storage_manager, "ROOT_HOME", root_home)

    with pytest.raises(ValueError, match="不允许清理"):
        storage_manager.cleanup_root_targets(["npm", "../outside"])

    assert (npm / "payload").read_bytes() == b"keep"


def test_root_cleanup_unlinks_symlink_without_following_it(tmp_path, monkeypatch):
    root_home = tmp_path / "root"
    outside = tmp_path / "outside"
    outside.mkdir()
    (outside / "keep").write_bytes(b"safe")
    root_home.mkdir()
    (root_home / ".npm").symlink_to(outside, target_is_directory=True)
    monkeypatch.setattr(storage_manager, "ROOT_HOME", root_home)

    result = storage_manager.cleanup_root_targets(["npm"])

    assert result["deleted_count"] == 1
    assert not (root_home / ".npm").exists()
    assert (outside / "keep").read_bytes() == b"safe"


def test_network_terse_parser_preserves_escaped_colons():
    assert network_manager.split_terse(r"wlan0:wifi:connected:office\:5g") == [
        "wlan0", "wifi", "connected", "office:5g",
    ]


def test_static_ipv4_validation_rejects_invalid_address():
    with pytest.raises(network_manager.NetworkManagerError, match="固定 IP 参数不正确"):
        network_manager.validate_ipv4("manual", "999.1.1.1/24", "192.168.1.1", [])


def test_static_ipv4_allows_isolated_lan_without_gateway():
    result = network_manager.validate_ipv4(
        "manual", "192.168.50.20/24", "", ["223.5.5.5"],
    )
    assert result == {
        "method": "manual", "address": "192.168.50.20/24",
        "gateway": "", "dns": ["223.5.5.5"],
    }


def test_network_apply_request_never_contains_shell_command():
    request = network_settings.NetworkApplyRequest(
        device="eth0", type="ethernet", method="auto", profile_name="有线 eth0",
    )
    assert request.model_dump()["device"] == "eth0"


def test_network_router_has_no_manual_restore_operation():
    paths = {route.path for route in network_settings.router.routes}
    assert not any(path.endswith("/rollback") for path in paths)
