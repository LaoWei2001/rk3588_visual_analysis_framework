import asyncio
import json
import subprocess

import pytest
from fastapi import HTTPException

from routers import system_settings as ss


def _completed(cmd, returncode=0, stdout="", stderr=""):
    return subprocess.CompletedProcess(cmd, returncode, stdout=stdout, stderr=stderr)


def test_enable_daily_reboot_writes_safe_systemd_units(tmp_path, monkeypatch):
    systemd_dir = tmp_path / "systemd"
    settings_file = tmp_path / "settings.json"
    calls = []

    def fake_run(cmd, timeout=15):
        calls.append(cmd)
        if len(cmd) > 1 and cmd[1] == "show":
            return _completed(cmd, stdout=(
                "LoadState=loaded\nActiveState=active\nUnitFileState=enabled\n"
                "NextElapseUSecRealtime=Wed 2026-08-12 03:30:00 CST\n"
            ))
        return _completed(cmd)

    monkeypatch.setattr(ss, "SYSTEMD_DIR", systemd_dir)
    monkeypatch.setattr(ss, "SETTINGS_FILE", settings_file)
    monkeypatch.setattr(ss, "_run", fake_run)

    result = asyncio.run(ss.set_daily_reboot(ss.DailyRebootRequest(enabled=True, time="03:30")))

    assert result["ok"] is True
    assert result["enabled"] is True
    assert result["active"] is True
    assert result["current_time_epoch_ms"] > 0
    assert isinstance(result["utc_offset_minutes"], int)
    assert "T" in result["current_time"]
    assert "OnCalendar=*-*-* 03:30:00" in (systemd_dir / ss.TIMER_UNIT).read_text()
    assert "Persistent=false" in (systemd_dir / ss.TIMER_UNIT).read_text()
    assert "ExecStart=/usr/bin/systemctl reboot" in (systemd_dir / ss.SERVICE_UNIT).read_text()
    assert json.loads(settings_file.read_text())["daily_reboot_time"] == "03:30"
    assert ss._systemctl_command("enable", "--now", ss.TIMER_UNIT) in calls
    assert ss._systemctl_command("restart", ss.TIMER_UNIT) in calls


def test_disable_daily_reboot_stops_and_disables_timer(tmp_path, monkeypatch):
    calls = []
    monkeypatch.setattr(ss, "SYSTEMD_DIR", tmp_path / "systemd")
    monkeypatch.setattr(ss, "SETTINGS_FILE", tmp_path / "settings.json")

    def fake_run(cmd, timeout=15):
        calls.append(cmd)
        if len(cmd) > 1 and cmd[1] == "show":
            return _completed(cmd, stdout="LoadState=loaded\nActiveState=inactive\nUnitFileState=disabled\n")
        return _completed(cmd)

    monkeypatch.setattr(ss, "_run", fake_run)
    result = asyncio.run(ss.set_daily_reboot(ss.DailyRebootRequest(enabled=False, time="23:59")))

    assert result["enabled"] is False
    assert ss._systemctl_command("disable", "--now", ss.TIMER_UNIT) in calls


def test_rejects_invalid_reboot_time(tmp_path, monkeypatch):
    monkeypatch.setattr(ss, "SYSTEMD_DIR", tmp_path / "systemd")
    monkeypatch.setattr(ss, "SETTINGS_FILE", tmp_path / "settings.json")
    with pytest.raises(HTTPException) as error:
        asyncio.run(ss.set_daily_reboot(ss.DailyRebootRequest(enabled=True, time="24:00")))
    assert error.value.status_code == 400
    assert not (tmp_path / "systemd").exists()


def test_systemctl_path_can_be_discovered_or_overridden(monkeypatch):
    monkeypatch.setenv("SYSTEMCTL_PATH", "/portable/system/bin/systemctl")
    assert ss._systemctl_command("reboot") == [
        "/portable/system/bin/systemctl", "reboot",
    ]
    assert "ExecStart=/portable/system/bin/systemctl reboot" in ss._service_content()
