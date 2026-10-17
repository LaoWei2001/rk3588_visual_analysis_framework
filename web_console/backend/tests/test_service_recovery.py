import asyncio
import os
import socket
import subprocess
import threading

import pytest

from services import process_manager as pm, runtime_state, service_watchdog


def test_web_stop_records_operation_and_storage_failure_does_not_block_stop(monkeypatch):
    from routers import process
    calls = []
    monkeypatch.setattr(process.pm, 'stop_app', lambda name: calls.append(name))
    assert asyncio.run(process.stop_app('demo')) == {'ok': True}
    row = process.recovery_history.snapshot()['events'][0]
    assert row['kind'] == 'manual_stop' and row['result'] == 'performed' and not row['fault_recovery']
    monkeypatch.setattr(process.recovery_history, 'record', lambda *args, **kwargs: (_ for _ in ()).throw(OSError('disk full')))
    assert asyncio.run(process.stop_app('demo')) == {'ok': True}
    assert calls == ['demo', 'demo']


@pytest.fixture
def app(tmp_path, monkeypatch):
    app = tmp_path / "demo"
    app.mkdir()
    (app / "run.config").write_text("production.json")
    (app / "run.systemd_unit").write_text(pm._app_unit_name("demo"))
    (app / "run.boot_id").write_text(pm._current_boot_id())
    (app / "run.pid").write_text("111")
    monkeypatch.setattr(pm, "APPS_ROOT", tmp_path)
    monkeypatch.setattr(pm, "_start_lock_path", tmp_path / ".start.lock")
    monkeypatch.setattr(pm, "_start_thread_lock", threading.RLock())
    monkeypatch.setattr(pm, "_start_lock_local", threading.local())
    monkeypatch.setattr(pm, "_processes", {})
    monkeypatch.setattr(runtime_state, "STATE_FILE", tmp_path / "state.json")
    monkeypatch.setattr(runtime_state, "LOCK_FILE", tmp_path / "state.lock")
    monkeypatch.setattr(pm.recovery_policy, 'POLICY_FILE', tmp_path / 'policy.json')
    return app


def test_new_pid_is_reassociated_after_crash(app, monkeypatch):
    monkeypatch.setattr(pm, "_systemd_unit_status", lambda unit: {
        "MainPID": "222", "ActiveState": "active", "NRestarts": "3",
    })
    monkeypatch.setattr(pm, "_pid_belongs_to_app", lambda name, pid: pid == 222)
    result = pm.get_status("demo")
    assert result["pid"] == 222 and result["restart_count"] == 3
    assert (app / "run.pid").read_text() == "222"
    assert result["config"] == "production.json"
    assert pm.get_running_app_context()["pid"] == 222


def test_restart_backoff_retains_single_instance_and_runtime_markers(app, monkeypatch):
    monkeypatch.setattr(pm, "_systemd_unit_status", lambda unit: {
        "MainPID": "0", "ActiveState": "activating", "SubState": "auto-restart", "NRestarts": "1",
    })
    result = pm.get_status("demo")
    assert result["recovery_state"] == "restarting"
    assert (app / "run.systemd_unit").exists()
    assert pm._find_running_app().app_name == "demo"


def test_manual_stop_during_backoff_prevents_resurrection(app, monkeypatch):
    from services.dataset_manager import dataset_manager
    monkeypatch.setattr(dataset_manager, "pause_app", lambda name: None)
    monkeypatch.setattr(pm, "_systemd_unit_status", lambda unit: {
        "MainPID": "0", "ActiveState": "activating", "SubState": "auto-restart",
    })
    stopped = []
    monkeypatch.setattr(pm, "_stop_systemd_unit", lambda unit: stopped.append(unit) or True)
    runtime_state.set_vision_autostart("demo", True)
    runtime_state.mark_vision_started("demo", "production.json")
    assert pm.stop_app("demo")
    assert stopped == [pm._app_unit_name("demo")]
    assert not (app / "run.systemd_unit").exists()
    assert runtime_state.get_vision_boot_target() is None


def test_failed_stop_keeps_ownership_but_persists_stop_intent(app, monkeypatch):
    from services.dataset_manager import dataset_manager
    monkeypatch.setattr(dataset_manager, "pause_app", lambda name: None)
    monkeypatch.setattr(pm, "_systemd_unit_status", lambda unit: {"ActiveState": "activating"})
    monkeypatch.setattr(pm, "_stop_systemd_unit", lambda unit: False)
    runtime_state.set_vision_autostart("demo", True)
    runtime_state.mark_vision_started("demo", "production.json")
    with pytest.raises(RuntimeError, match="无法停止"):
        pm.stop_app("demo")
    assert (app / "run.systemd_unit").exists()
    assert runtime_state.get_vision_boot_target() is None


@pytest.mark.parametrize("supported", [False, True])
def test_watchdog_capability_and_parent_environment(app, monkeypatch, supported):
    # Compatibility only: application recovery is disabled by default.
    pm.recovery_policy.write({**pm.recovery_policy.DEFAULTS, 'app_restart_enabled': True, 'app_stall_enabled': True})
    monkeypatch.setattr(pm, "_supports_service_watchdog", lambda *args: supported)
    calls = []
    def run(command, **kwargs):
        calls.append(command)
        return subprocess.CompletedProcess(command, 0, "", "")
    monkeypatch.setattr(pm.subprocess, "run", run)
    monkeypatch.setattr(pm.subprocess, "Popen", lambda command, **kw: calls.append(command))
    pm._start_systemd_app(app / "vision_analysis", app / "assets/production.json", app,
                          {"WATCHDOG_PID": "1", "WATCHDOG_USEC": "1", "NOTIFY_SOCKET": "/parent"},
                          pm._app_unit_name("demo"))
    command = calls[0]
    assert "--property=Restart=always" in command
    assert "--property=StartLimitIntervalSec=0" in command
    assert ("--property=WatchdogSec=30" in command) == supported
    assert not any(arg.startswith(("--setenv=WATCHDOG_", "--setenv=NOTIFY_SOCKET=")) for arg in command)
    assert '--setenv=RK_SERVICE_TASK_TIMEOUT_MS=120000' in command
    assert calls[1][0] == "journalctl" and "--follow" in calls[1]


def test_console_notification_uses_real_unix_socket(tmp_path, monkeypatch):
    path = str(tmp_path / "notify.sock")
    with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as listener:
        listener.bind(path)
        listener.settimeout(1)
        monkeypatch.setenv("NOTIFY_SOCKET", path)
        assert service_watchdog.notify("WATCHDOG=1")
        assert listener.recv(128) == b"WATCHDOG=1"


def test_console_startup_stall_stops_heartbeats(monkeypatch):
    monkeypatch.setenv("WATCHDOG_USEC", "1000")
    monkeypatch.setenv("WATCHDOG_PID", str(os.getpid()))
    monkeypatch.setenv("NOTIFY_SOCKET", "/test")
    messages = []
    monkeypatch.setattr(service_watchdog, "notify", lambda msg: messages.append(msg))
    async def run():
        await service_watchdog.heartbeat(asyncio.Event(), startup_timeout=-1)
    asyncio.run(run())
    assert len(messages) == 1 and messages[0].startswith("STATUS=")
