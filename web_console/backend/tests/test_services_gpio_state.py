import asyncio
import subprocess
from contextlib import contextmanager

from routers import services


def _completed(cmd, returncode=0, stdout="", stderr=""):
    return subprocess.CompletedProcess(cmd, returncode, stdout=stdout, stderr=stderr)


def test_gpio_state_status_is_system_scoped_and_uses_real_systemd_state(
    tmp_path, monkeypatch
):
    unit = tmp_path / "rk3588-gpio-restore.service"
    unit.write_text("[Service]\nType=oneshot\n", encoding="utf-8")

    def fake_run(cmd, timeout=15):
        assert cmd[:3] == ["systemctl", "show", "rk3588-gpio-restore.service"]
        return _completed(cmd, stdout=(
            "LoadState=loaded\n"
            "ActiveState=active\n"
            "SubState=exited\n"
            "UnitFileState=enabled\n"
            "NRestarts=0\n"
            "ActiveEnterTimestampMonotonic=1000000\n"
            "WorkingDirectory=\n"
            "Environment=\n"
            f"FragmentPath={unit}\n"
        ))

    monkeypatch.setattr(services, "_run", fake_run)
    result = asyncio.run(services.list_services())
    gpio = next(item for item in result if item["key"] == "gpio_state")

    assert gpio["scope"] == "system"
    assert gpio["installed"] is True
    assert gpio["path_ok"] is True
    assert gpio["active_state"] == "active"
    assert gpio["autostart"] is True
    assert gpio["desired_running"] is True
    assert gpio["bound_app"] is None


def test_gpio_state_start_enables_and_restores_without_running_app(monkeypatch):
    calls = []

    def fake_run(cmd, timeout=15):
        calls.append(cmd)
        return _completed(cmd)

    monkeypatch.setattr(services, "_run", fake_run)
    result = asyncio.run(services.control_service("gpio_state", "start"))

    assert result == {
        "ok": True,
        "unit": "rk3588-gpio-restore.service",
        "started": True,
    }
    assert calls == [
        ["systemctl", "enable", "rk3588-gpio-restore.service"],
        ["systemctl", "restart", "rk3588-gpio-restore.service"],
    ]


def test_gpio_state_stop_disables_service(monkeypatch):
    calls = []

    def fake_run(cmd, timeout=15):
        calls.append(cmd)
        return _completed(cmd)

    monkeypatch.setattr(services, "_run", fake_run)
    result = asyncio.run(services.control_service("gpio_state", "stop"))

    assert result["ok"] is True
    assert result["started"] is False
    assert calls == [[
        "systemctl", "disable", "--now", "rk3588-gpio-restore.service",
    ]]


def test_gpio_state_autostart_toggle_controls_systemd_directly(monkeypatch):
    calls = []

    def fake_run(cmd, timeout=15):
        calls.append(cmd)
        return _completed(cmd)

    monkeypatch.setattr(services, "_run", fake_run)
    enabled = asyncio.run(services.set_service_autostart(
        "gpio_state", services.AutostartReq(enabled=True)
    ))
    disabled = asyncio.run(services.set_service_autostart(
        "gpio_state", services.AutostartReq(enabled=False)
    ))

    assert enabled["autostart"] is True
    assert disabled["autostart"] is False
    assert calls == [
        ["systemctl", "enable", "--now", "rk3588-gpio-restore.service"],
        ["systemctl", "disable", "--now", "rk3588-gpio-restore.service"],
    ]


def test_visual_app_sync_never_touches_gpio_system_service(monkeypatch):
    checked = []

    @contextmanager
    def fake_runtime_lock():
        yield

    def fake_status(key):
        checked.append(key)
        assert key != "gpio_state"
        return {
            "active_state": "inactive",
            "autostart": False,
            "desired_running": False,
        }

    monkeypatch.setattr(services.pm, "runtime_lock", fake_runtime_lock)
    monkeypatch.setattr(services.pm, "get_running_app_context", lambda: {
        "app": "demo",
        "app_dir": "/opt/ai_apps/demo",
        "config": "config.json",
    })
    monkeypatch.setattr(services, "_status", fake_status)

    result = services.sync_services_for_running_app()

    assert "gpio_state" not in checked
    assert result == {"updated": [], "errors": []}
