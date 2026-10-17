"""设备级系统设置。

提供系统时间、时区和每日定时重启。计划由 systemd timer 执行，不依赖浏览器。
系统命令使用独立参数，不经过 shell；手动设时与时区修改在同一进程锁内串行。
"""
from __future__ import annotations

import json
import asyncio
import os
import re
import shutil
import subprocess
import threading
from datetime import datetime, timezone as datetime_timezone
from pathlib import Path
from typing import Any, Dict, List, Optional
from zoneinfo import TZPATH, ZoneInfo, available_timezones

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel, StrictInt
from services.data_dir import APPS_ROOT
from routers.network_settings import require_network_admin
from services.runtime_loader import load_runtime


router = APIRouter()

SYSTEMD_DIR = Path(os.environ.get("SYSTEMD_DIR", "/etc/systemd/system"))
SETTINGS_FILE = Path(os.environ.get(
    "SYSTEM_SETTINGS_FILE", str(APPS_ROOT / ".console_system_settings.json")
))
SERVICE_UNIT = "rk3588-daily-reboot.service"
TIMER_UNIT = "rk3588-daily-reboot.timer"
DEFAULT_REBOOT_TIME = "04:00"
_TIME_RE = re.compile(r"^(?:[01]\d|2[0-3]):[0-5]\d$")
_clock_lock = threading.Lock()


class DailyRebootRequest(BaseModel):
    enabled: bool
    time: str = DEFAULT_REBOOT_TIME


class TimezoneRequest(BaseModel):
    timezone: str


class SystemTimeRequest(BaseModel):
    timezone: str
    local_time: Optional[str] = None
    epoch_ms: Optional[StrictInt] = None
    disable_ntp: bool = False


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
    return load_runtime('reboot_coordinator').daily_service_content(systemctl)


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
            "--property=LoadState,ActiveState,UnitFileState,NextElapseUSecRealtime,TimersCalendar",
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
    calendar = values.get("TimersCalendar", "")
    match = re.search(r"OnCalendar=\*-\*-\* (\d{2}:\d{2}):00(?:\s|;|\})", calendar)
    if match:
        result["time"] = match[1]
    return result


def _response() -> Dict[str, Any]:
    settings = _read_settings()
    timer = _timer_status()
    now = datetime.now().astimezone()
    utc_offset = now.utcoffset()
    return {
        **settings,
        **timer,
        "configured_enabled": settings["enabled"],
        "enabled": timer["active"] if not timer["error"] else settings["enabled"],
        "timezone": _timezone_name(),
        "current_time": now.isoformat(timespec="seconds"),
        "current_time_epoch_ms": int(now.timestamp() * 1000),
        "utc_offset_minutes": int(utc_offset.total_seconds() // 60) if utc_offset else 0,
        **_clock_status(),
    }


def _clock_status() -> Dict[str, Any]:
    status = {"ntp_enabled": None, "ntp_synchronized": None, "can_ntp": None, "time_error": None}
    try:
        command = _command_path("timedatectl", "TIMEDATECTL_PATH")
        # 板载旧版 timedatectl 不支持逗号分隔属性，使用重复选项兼容。
        result = _run([command, "show", "--property=CanNTP", "--property=NTP", "--property=NTPSynchronized"], timeout=5)
        if result.returncode:
            raise RuntimeError((result.stderr or result.stdout or "无法读取自动校时状态").strip())
        values = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
        for field, key in (("ntp_enabled", "NTP"), ("ntp_synchronized", "NTPSynchronized"), ("can_ntp", "CanNTP")):
            status[field] = {"yes": True, "no": False}.get(values.get(key))
        if status["ntp_enabled"] is None:
            raise RuntimeError("系统未返回自动校时状态，暂不能修改时间")
    except Exception as exc:
        status["time_error"] = str(exc)
    return status


def _time_target(req: SystemTimeRequest) -> str:
    if (req.local_time is None) == (req.epoch_ms is None):
        raise HTTPException(400, "请提供日期时间或电脑时间戳，不能同时提供")
    try:
        if req.epoch_ms is not None:
            moment = datetime.fromtimestamp(req.epoch_ms / 1000, datetime_timezone.utc)
        else:
            if not re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}", req.local_time or ""):
                raise ValueError("日期时间格式必须为 YYYY-MM-DDTHH:MM:SS")
            local = datetime.strptime(req.local_time, "%Y-%m-%dT%H:%M:%S")
            zone = ZoneInfo(req.timezone)
            moment = local.replace(tzinfo=zone)
            if moment.astimezone(datetime_timezone.utc).astimezone(zone).replace(tzinfo=None) != local:
                raise ValueError("该日期时间处于夏令时跳过区间，请选择其他时间")
            if moment.utcoffset() != local.replace(tzinfo=zone, fold=1).utcoffset():
                raise ValueError("该日期时间处于夏令时重叠区间，请选择其他时间或与电脑同步")
        if moment.year < 1970:
            raise ValueError("系统时间不能早于 1970 年")
        return "@" + str(int(moment.timestamp()))
    except (ValueError, OverflowError, OSError, KeyError) as exc:
        raise HTTPException(400, f"无效的系统时间：{exc}") from exc


def _set_system_time(req: SystemTimeRequest) -> Dict[str, Any]:
    if req.timezone not in available_timezones():
        raise HTTPException(400, "无效的 IANA 时区")
    target = _time_target(req)
    with _clock_lock:
        if _timezone_name() != req.timezone:
            raise HTTPException(409, "设备时区已变化，请重新同步后再设置时间")
        state = _clock_status()
        if state["time_error"]:
            raise HTTPException(503, state["time_error"])
        if state["ntp_enabled"] and not req.disable_ntp:
            raise HTTPException(409, "自动校时已开启，请确认关闭自动校时后再设置时间")
        timedatectl = _command_path("timedatectl", "TIMEDATECTL_PATH")
        stopped_ntp = False
        try:
            if state["ntp_enabled"]:
                result = _run([timedatectl, "set-ntp", "false"])
                if result.returncode:
                    raise RuntimeError((result.stderr or result.stdout or "关闭自动校时失败").strip())
                stopped_ntp = True
            result = _run([timedatectl, "set-time", target])
            if result.returncode:
                raise RuntimeError((result.stderr or result.stdout or "设置系统时间失败").strip())
        except Exception as exc:
            rollback_error = ""
            if stopped_ntp:
                try:
                    restored = _run([timedatectl, "set-ntp", "true"])
                    if restored.returncode:
                        raise RuntimeError((restored.stderr or restored.stdout or "恢复失败").strip())
                except Exception as rollback:
                    rollback_error = f"；恢复自动校时也失败：{rollback}"
            raise HTTPException(500, f"设置系统时间失败：{exc}{rollback_error}") from exc
        return {"ok": True, **_response()}


@router.put("/system/time")
async def set_system_time(req: SystemTimeRequest, _admin: None = Depends(require_network_admin)):
    return await asyncio.to_thread(_set_system_time, req)


@router.get("/system/daily-reboot")
async def get_daily_reboot():
    return await asyncio.to_thread(_response)


@router.get("/system/timezones")
async def get_timezones():
    zones = sorted(
        zone for zone in available_timezones()
        if not zone.startswith(("posix/", "right/")) and zone != "localtime"
    )
    return {"timezones": zones}


@router.put("/system/timezone")
async def set_timezone(req: TimezoneRequest):
    return await asyncio.to_thread(_set_timezone, req)


def _set_timezone(req: TimezoneRequest):
    timezone = req.timezone.strip()
    if timezone not in available_timezones() or timezone.startswith(("posix/", "right/")):
        raise HTTPException(400, "无效的 IANA 时区")
    try:
        timedatectl = _command_path("timedatectl", "TIMEDATECTL_PATH")
        with _clock_lock:
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
