import asyncio
import json
import subprocess
from datetime import datetime
from zoneinfo import ZoneInfo

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
    service = (systemd_dir / ss.SERVICE_UNIT).read_text()
    assert "ExecStart=/usr/bin/python3 -m rk3588_runtime.reboot_coordinator --scheduled" in service
    assert '--record-scheduled-reboot' not in service and '--scheduled-result' not in service
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
    assert 'Environment="SYSTEMCTL_PATH=/portable/system/bin/systemctl"' in ss._service_content()


@pytest.mark.parametrize('active', [False, True])
def test_external_timer_state_and_calendar_override_legacy_settings(tmp_path, monkeypatch, active):
    monkeypatch.setattr(ss, 'SYSTEMD_DIR', tmp_path)
    monkeypatch.setattr(ss, '_read_settings', lambda: {'enabled': not active, 'time': '04:00'})
    monkeypatch.setattr(ss, '_timezone_name', lambda: 'UTC')

    def read_only(cmd, timeout=15):
        assert cmd[1] == 'show'
        return _completed(cmd, stdout=(
            f'LoadState=loaded\nActiveState={"active" if active else "inactive"}\n'
            'UnitFileState=enabled\nTimersCalendar={ OnCalendar=*-*-* 12:34:00 ; next_elapse=... }\n'))

    monkeypatch.setattr(ss, '_run', read_only)
    result = asyncio.run(ss.get_daily_reboot())
    assert result['enabled'] is active
    assert result['configured_enabled'] is not active
    assert result['time'] == '12:34'


@pytest.fixture
def clock(monkeypatch):
    state = {'ntp': False, 'fail_time': False, 'fail_restore': False, 'fail_read': False}
    calls = []
    monkeypatch.setattr(ss, '_timezone_name', lambda: 'Asia/Shanghai')
    monkeypatch.setattr(ss, '_command_path', lambda *_: '/isolated/timedatectl')
    monkeypatch.setattr(ss, '_response', lambda: {'timezone': 'Asia/Shanghai', 'ntp_enabled': state['ntp']})

    def fake_run(cmd, timeout=15):
        calls.append(cmd)
        if cmd[1] == 'show':
            assert cmd[2:] == ['--property=CanNTP', '--property=NTP', '--property=NTPSynchronized']
            return _completed(cmd, returncode=1 if state['fail_read'] else 0,
                              stdout=f'CanNTP=yes\nNTP={"yes" if state["ntp"] else "no"}\nNTPSynchronized=no\n')
        if cmd[1] == 'set-ntp':
            if cmd[2] == 'true' and state['fail_restore']:
                return _completed(cmd, returncode=1, stderr='cannot restart NTP')
            state['ntp'] = cmd[2] == 'true'
        if cmd[1] == 'set-time' and state['fail_time']:
            return _completed(cmd, returncode=1, stderr='cannot set clock')
        return _completed(cmd)

    monkeypatch.setattr(ss, '_run', fake_run)
    return state, calls


def test_manual_time_uses_device_timezone_and_absolute_epoch_without_shell(clock):
    state, calls = clock
    result = asyncio.run(ss.set_system_time(ss.SystemTimeRequest(
        timezone='Asia/Shanghai', local_time='2026-10-20T12:34:56')))
    epoch = int(datetime(2026, 10, 20, 12, 34, 56, tzinfo=ZoneInfo('Asia/Shanghai')).timestamp())
    assert result['ok'] and result['ntp_enabled'] is False
    assert calls[-1] == ['/isolated/timedatectl', 'set-time', f'@{epoch}']
    assert not any(cmd[1] == 'set-ntp' for cmd in calls)


def test_computer_epoch_is_independent_of_browser_and_device_timezones(clock):
    _, calls = clock
    req = ss.SystemTimeRequest(timezone='Asia/Shanghai', epoch_ms=1792141108123)
    asyncio.run(ss.set_system_time(req))
    assert calls[-1] == ['/isolated/timedatectl', 'set-time', '@1792141108']


@pytest.mark.parametrize('payload', [
    {'local_time': '2026-02-30T12:00:00'}, {'local_time': '2026-10-20T25:00:00'},
    {'local_time': '2026-10-20T12:00:00; reboot'}, {'local_time': '1969-10-20T12:00:00'},
    {'local_time': '2026-10-20T12:00:00', 'epoch_ms': 1234567890000}, {},
    {'epoch_ms': 10**30},
])
def test_invalid_clock_requests_never_execute_commands(clock, payload):
    _, calls = clock
    with pytest.raises(HTTPException) as error:
        asyncio.run(ss.set_system_time(ss.SystemTimeRequest(timezone='Asia/Shanghai', **payload)))
    assert error.value.status_code == 400 and not calls


@pytest.mark.parametrize('local', ['2026-03-08T02:30:00', '2026-11-01T01:30:00'])
def test_dst_gap_and_ambiguous_local_times_are_rejected_without_setting_clock(clock, local):
    _, calls = clock
    with pytest.raises(HTTPException, match='夏令时'):
        ss._time_target(ss.SystemTimeRequest(timezone='America/New_York', local_time=local))
    assert not calls


def test_changed_device_timezone_rejects_stale_manual_time(clock):
    _, calls = clock
    with pytest.raises(HTTPException) as error:
        asyncio.run(ss.set_system_time(ss.SystemTimeRequest(timezone='UTC', epoch_ms=1792141108000)))
    assert error.value.status_code == 409 and not calls


def test_ntp_enabled_requires_explicit_disable_and_is_stopped_before_setting_time(clock):
    state, calls = clock
    state['ntp'] = True
    with pytest.raises(HTTPException) as error:
        asyncio.run(ss.set_system_time(ss.SystemTimeRequest(timezone='Asia/Shanghai', epoch_ms=1792141108000)))
    assert error.value.status_code == 409 and all(cmd[1] == 'show' for cmd in calls)
    result = asyncio.run(ss.set_system_time(ss.SystemTimeRequest(
        timezone='Asia/Shanghai', epoch_ms=1792141108000, disable_ntp=True)))
    assert result['ntp_enabled'] is False
    assert [cmd[1:] for cmd in calls[-2:]] == [['set-ntp', 'false'], ['set-time', '@1792141108']]


@pytest.mark.parametrize('fail_restore', [False, True])
def test_failed_clock_change_restores_previous_ntp_or_reports_rollback_failure(clock, fail_restore):
    state, calls = clock
    state.update(ntp=True, fail_time=True, fail_restore=fail_restore)
    with pytest.raises(HTTPException) as error:
        asyncio.run(ss.set_system_time(ss.SystemTimeRequest(
            timezone='Asia/Shanghai', epoch_ms=1792141108000, disable_ntp=True)))
    assert error.value.status_code == 500
    assert calls[-1][1:] == ['set-ntp', 'true']
    assert ('恢复自动校时也失败' in error.value.detail) is fail_restore
    assert state['ntp'] is (not fail_restore)


def test_unknown_ntp_state_prevents_time_writes(clock):
    state, calls = clock
    state['fail_read'] = True
    with pytest.raises(HTTPException) as error:
        asyncio.run(ss.set_system_time(ss.SystemTimeRequest(timezone='Asia/Shanghai', epoch_ms=1792141108000)))
    assert error.value.status_code == 503 and all(cmd[1] == 'show' for cmd in calls)


def test_time_write_requires_admin_account(clock):
    from fastapi import Request
    route = next(route for route in ss.router.routes if route.path == '/system/time')
    assert any(dep.call is ss.require_network_admin for dep in route.dependant.dependencies)
    request = Request({'type': 'http', 'state': {
        'session': {'username': 'nonexistent-clock-test-account'},
    }})
    with pytest.raises(HTTPException) as error:
        ss.require_network_admin(request)
    assert error.value.status_code == 403 and not clock[1]
