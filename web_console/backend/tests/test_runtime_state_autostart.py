from services import runtime_state


def _isolated_state(tmp_path, monkeypatch):
    state_file = tmp_path / ".console_runtime_state.json"
    monkeypatch.setattr(runtime_state, "STATE_FILE", state_file)
    monkeypatch.setattr(
        runtime_state,
        "LOCK_FILE",
        state_file.with_suffix(state_file.suffix + ".lock"),
    )


def test_vision_boot_target_requires_autostart_and_running_intent(tmp_path, monkeypatch):
    _isolated_state(tmp_path, monkeypatch)

    runtime_state.set_vision_autostart("demo", True)
    assert runtime_state.get_vision_boot_target() is None

    runtime_state.mark_vision_started("demo", "deploy", "production.json")
    assert runtime_state.get_vision_boot_target() == {
        "app": "demo",
        "mode": "deploy",
        "config": "production.json",
    }

    runtime_state.mark_vision_stopped("demo")
    assert runtime_state.get_vision_boot_target() is None


def test_background_service_restores_only_when_both_flags_are_true(tmp_path, monkeypatch):
    _isolated_state(tmp_path, monkeypatch)

    runtime_state.set_service_autostart("unified_upload", True)
    assert runtime_state.get_service_settings("unified_upload") == {
        "autostart": True,
        "desired_running": False,
    }

    runtime_state.mark_service_started("unified_upload")
    assert runtime_state.get_service_settings("unified_upload") == {
        "autostart": True,
        "desired_running": True,
    }
