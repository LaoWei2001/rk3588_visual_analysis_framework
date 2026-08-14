"""设备级系统设置。

当前只提供每日定时重启。计划由 systemd timer 执行，因此不依赖浏览器保持打开，
也不依赖 Web 控制台进程内的定时线程。控制台服务以 root 运行，能够安全地安装固定名称、
固定内容的单元；请求参数只允许 ``HH:MM``，不会进入 shell。
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List
from zoneinfo import TZPATH, available_timezones

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel
from services.data_dir import APPS_ROOT


router = APIRouter()

SYSTEMD_DIR = Path(os.environ.get("SYSTEMD_DIR", "/etc/systemd/system"))
SETTINGS_FILE = Path(os.environ.get(
    "SYSTEM_SETTINGS_FILE", str(APPS_ROOT / ".console_system_settings.json")
))
SERVICE_UNIT = "rk3588-daily-reboot.service"
TIMER_UNIT = "rk3588-daily-reboot.timer"
DEFAULT_REBOOT_TIME = "04:00"
_TIME_RE = re.compile(r"^(?:[01]\d|2[0-3]):[0-5]\d$")


class DailyRebootRequest(BaseModel):
    enabled: bool
    time: str = DEFAULT_REBOOT_TIME


class TimezoneRequest(BaseModel):
    timezone: str


def _run(cmd: List[str], timeout: int = 15) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def _command_path(name: str, override_env: str) -> str:
    """返回当前盒子上的命令绝对路径，不假定命令安装在 /usr/bin。"""
    override = os.environ.get(override_env, "").strip()
    if override:
        path = Path(override)
        if not path.is_absolute():
            raise RuntimeError(f"{override_env} 必须是绝对路径")
        return str(path)
    discovered = shutil.which(name)
    if not discovered:
        raise RuntimeError(f"当前系统找不到 {name}，该功能需要 systemd")
    return str(Path(discovered).resolve())


def _systemctl_command(*args: str) -> List[str]:
    return [_command_path("systemctl", "SYSTEMCTL_PATH"), *args]


def _normalize_time(value: Any) -> str:
    text = str(value or "").strip()
    if not _TIME_RE.fullmatch(text):
        raise ValueError("重启时间格式必须为 HH:MM（00:00 到 23:59）")
    return text


def _read_settings() -> Dict[str, Any]:
    try:
        raw = json.loads(SETTINGS_FILE.read_text(encoding="utf-8"))
    except (OSError, ValueError, TypeError):
        raw = {}
    try:
        reboot_time = _normalize_time(raw.get("daily_reboot_time", DEFAULT_REBOOT_TIME))
    except ValueError:
        reboot_time = DEFAULT_REBOOT_TIME
    return {
        "enabled": bool(raw.get("daily_reboot_enabled", False)),
        "time": reboot_time,
    }


def _write_settings(enabled: bool, reboot_time: str) -> None:
    SETTINGS_FILE.parent.mkdir(parents=True, exist_ok=True)
    data = {
        "version": 1,
        "daily_reboot_enabled": bool(enabled),
        "daily_reboot_time": reboot_time,
    }
    temporary = SETTINGS_FILE.with_suffix(SETTINGS_FILE.suffix + ".tmp")
    temporary.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, SETTINGS_FILE)


def _service_content() -> str:
    systemctl = _command_path("systemctl", "SYSTEMCTL_PATH")
    return (
        "[Unit]\n"
        "Description=RK3588 Daily Scheduled Reboot\n\n"
        "[Service]\n"
        "Type=oneshot\n"
        f"ExecStart={systemctl} reboot\n"
    )


def _timer_content(reboot_time: str) -> str:
    return (
        "[Unit]\n"
        "Description=RK3588 Daily Scheduled Reboot Timer\n\n"
        "[Timer]\n"
        f"OnCalendar=*-*-* {reboot_time}:00\n"
        # 不追补错过的时刻，避免设备在设定时间之后开机时立即再次重启。
        "Persistent=false\n"
        "AccuracySec=1min\n"
        f"Unit={SERVICE_UNIT}\n\n"
        "[Install]\n"
        "WantedBy=timers.target\n"
    )


def _write_units(reboot_time: str) -> None:
    SYSTEMD_DIR.mkdir(parents=True, exist_ok=True)
    (SYSTEMD_DIR / SERVICE_UNIT).write_text(_service_content(), encoding="utf-8")
    (SYSTEMD_DIR / TIMER_UNIT).write_text(_timer_content(reboot_time), encoding="utf-8")


def _timezone_name() -> str:
    # /etc/localtime 才是 libc 与 systemd 真正使用的时区数据。
    # 部分板卡修改时区后不会同步更新 /etc/timezone（本机曾出现 localtime=Asia/Shanghai、
    # timezone=Etc/UTC），因此不能优先相信后者。
    localtime_file = Path(os.environ.get("LOCALTIME_FILE", "/etc/localtime"))
    for zoneinfo_entry in TZPATH:
        try:
            zoneinfo_root = Path(zoneinfo_entry).resolve(strict=True)
            localtime_target = localtime_file.resolve(strict=True)
            relative = localtime_target.relative_to(zoneinfo_root)
            if relative.parts:
                return relative.as_posix()
        except (OSError, ValueError):
            continue

    try:
        timedatectl = _command_path("timedatectl", "TIMEDATECTL_PATH")
        shown = _run([timedatectl, "show", "--property=Timezone", "--value"], timeout=5)
        value = shown.stdout.strip() if shown.returncode == 0 else ""
        if value:
            return value
    except Exception:
        pass

    # 仅作为不支持 timedatectl、且 /etc/localtime 不是标准 zoneinfo 链接时的兼容回退。
    try:
        timezone_file = Path(os.environ.get("TIMEZONE_FILE", "/etc/timezone"))
        value = timezone_file.read_text(encoding="utf-8").strip()
        if value:
            return value
    except OSError:
        pass
    return str(datetime.now().astimezone().tzinfo or "local")


def _timer_status() -> Dict[str, Any]:
    result = {
        "installed": (SYSTEMD_DIR / TIMER_UNIT).is_file(),
        "active": False,
        "unit_enabled": False,
        "next_run": None,
        "error": None,
    }
    try:
        shown = _run(_systemctl_command(
            "show", TIMER_UNIT,
            "--property=LoadState,ActiveState,UnitFileState,NextElapseUSecRealtime",
        ))
    except Exception as exc:
        result["error"] = str(exc)
        return result
    if shown.returncode != 0:
        detail = (shown.stderr or shown.stdout or "").strip()
        if detail:
            result["error"] = detail
        return result

    values: Dict[str, str] = {}
    for line in shown.stdout.splitlines():
        key, _, value = line.partition("=")
        values[key] = value
    result.update({
        "installed": values.get("LoadState") == "loaded",
        "active": values.get("ActiveState") == "active",
        "unit_enabled": values.get("UnitFileState") in ("enabled", "enabled-runtime"),
        "next_run": values.get("NextElapseUSecRealtime") or None,
    })
    return result


def _response() -> Dict[str, Any]:
    settings = _read_settings()
    now = datetime.now().astimezone()
    utc_offset = now.utcoffset()
    return {
        **settings,
        **_timer_status(),
        "timezone": _timezone_name(),
        "current_time": now.isoformat(timespec="seconds"),
        "current_time_epoch_ms": int(now.timestamp() * 1000),
        "utc_offset_minutes": int(utc_offset.total_seconds() // 60) if utc_offset else 0,
    }


@router.get("/system/daily-reboot")
async def get_daily_reboot():
    return _response()


@router.get("/system/timezones")
async def get_timezones():
    zones = sorted(
        zone for zone in available_timezones()
        if not zone.startswith(("posix/", "right/")) and zone != "localtime"
    )
    return {"timezones": zones}


@router.put("/system/timezone")
async def set_timezone(req: TimezoneRequest):
    timezone = req.timezone.strip()
    if timezone not in available_timezones() or timezone.startswith(("posix/", "right/")):
        raise HTTPException(400, "无效的 IANA 时区")
    try:
        timedatectl = _command_path("timedatectl", "TIMEDATECTL_PATH")
        result = _run([timedatectl, "set-timezone", timezone])
    except Exception as exc:
        raise HTTPException(500, f"设置时区失败：{exc}")
    if result.returncode != 0:
        raise HTTPException(500, (result.stderr or result.stdout or "设置时区失败").strip())
    return {"ok": True, **_response()}


@router.put("/system/daily-reboot")
async def set_daily_reboot(req: DailyRebootRequest):
    try:
        reboot_time = _normalize_time(req.time)
        _write_units(reboot_time)
        reloaded = _run(_systemctl_command("daemon-reload"))
        if reloaded.returncode != 0:
            raise RuntimeError((reloaded.stderr or reloaded.stdout or "daemon-reload 失败").strip())

        if req.enabled:
            changed = _run(_systemctl_command("enable", "--now", TIMER_UNIT))
            if changed.returncode == 0:
                # enable --now 对已经 active 的 timer 不会重新装载新 OnCalendar。
                changed = _run(_systemctl_command("restart", TIMER_UNIT))
        else:
            changed = _run(_systemctl_command("disable", "--now", TIMER_UNIT))
        if changed.returncode != 0:
            raise RuntimeError((changed.stderr or changed.stdout or "更新定时器失败").strip())

        _write_settings(req.enabled, reboot_time)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc))
    except PermissionError:
        raise HTTPException(status_code=500, detail="安装定时重启失败：Web 控制台需要以 root 运行")
    except Exception as exc:
        raise HTTPException(status_code=500, detail=f"安装定时重启失败：{exc}")
    return {"ok": True, **_response()}
