"""
services.py — 网页托管「板端后台微服务」(systemd 单元)。

设计要点:
  - systemd 是唯一的进程管家, 本路由只是它的遥控器 + 仪表盘。
  - deploy.sh(开发者命令行) 与 web 操作的是同一套**同名**单元(ota_agent.service /
    unified_upload.service), 互相覆盖同一文件、不会双开, 因此并存不冲突。
  - 控制台以 root 运行(rk3588-console.service), 可直接 systemctl / 写单元文件。
  - 只管两个 python 服务; 二进制(vision_app)仍由 process_manager 按 App 托管(维持分工)。

安全: 用户只传白名单 key, 单元名/动作均从服务端常量取, 绝不把任意串塞进 systemctl。
"""
from __future__ import annotations

import os
import shlex
import subprocess
from pathlib import Path
from typing import Any, Dict, List, Optional

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel

from services import process_manager as pm
from services import runtime_state
from services.data_dir import data_dir, migrate_app_data

APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))
SYSTEMD_DIR = Path("/etc/systemd/system")

router = APIRouter()

# 受管单元白名单
MANAGED: Dict[str, Dict[str, str]] = {
    "ota_agent": {
        "unit": "ota_agent.service",
        "label": "模型 OTA 升级服务",
        "subdir": "services/model_update",
    },
    "unified_upload": {
        "unit": "unified_upload.service",
        "label": "告警上报服务",
        "subdir": "services/upload",
    },
}


def _svc(key: str) -> Dict[str, str]:
    s = MANAGED.get(key)
    if not s:
        raise HTTPException(status_code=404, detail=f"未知服务: {key}")
    return s


def _run(cmd: List[str], timeout: int = 15) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def _systemd_env(value: str) -> str:
    """转义 systemd Environment="..." 中的值。"""
    return (value.replace("\\", "\\\\").replace('"', '\\"')
            .replace("\n", "\\n").replace("\r", "\\r"))


def _systemd_quote(value: str) -> str:
    return f'"{_systemd_env(value)}"'


def _unit_content(key: str, app_dir: Path, config_name: str) -> str:
    """生成绑定当前视觉 App 的单元；同时设置持久数据目录的环境变量。"""
    app_name = app_dir.name
    d = data_dir(app_name)
    event_store = d / "event_store"
    upload_data = d
    ota_config_file = d / "ota_config.json"

    if key == "ota_agent":
        return (
            "[Unit]\n"
            "Description=Edge Box OTA Agent\n"
            "After=network.target\n"
            "StartLimitIntervalSec=120\n"
            "StartLimitBurst=5\n\n"
            "[Service]\n"
            "Type=simple\n"
            f"WorkingDirectory={app_dir / 'services/model_update'}\n"
            f'Environment="ASSETS_DIR={_systemd_env(str(app_dir / "assets"))}"\n'
            f'Environment="CONFIG_FILE={_systemd_env(config_name)}"\n'
            f'Environment="OTA_CONFIG_FILE={_systemd_env(str(ota_config_file))}"\n'
            f"ExecStart=/usr/bin/python3 -u {_systemd_quote(str(app_dir / 'services/model_update/ota_agent.py'))}\n"
            "Restart=always\n"
            "RestartSec=3\n"
            "User=root\n\n"
            "[Install]\n"
            "WantedBy=multi-user.target\n"
        )
    # unified_upload
    return (
        "[Unit]\n"
        "Description=RK3588 Unified Upload Service\n"
        "After=network-online.target\n"
        "Wants=network-online.target\n"
        "StartLimitIntervalSec=120\n"
        "StartLimitBurst=5\n\n"
        "[Service]\n"
        "Type=simple\n"
        f"WorkingDirectory={app_dir / 'services/upload'}\n"
        f'Environment="UPLOAD_DATA_DIR={_systemd_env(str(upload_data))}"\n'
        f'Environment="EVENT_STORE_DIR={_systemd_env(str(event_store))}"\n'
        "ExecStart=/usr/bin/python3 -u main.py\n"
        "Restart=always\n"
        "RestartSec=5\n"
        "User=root\n\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n"
    )


def _environment_value(raw: str, name: str) -> Optional[str]:
    try:
        entries = shlex.split(raw)
    except ValueError:
        entries = raw.split()
    prefix = name + "="
    for entry in entries:
        if entry.startswith(prefix):
            return entry[len(prefix):]
    return None


def _status(key: str) -> Dict[str, Any]:
    unit = MANAGED[key]["unit"]
    props = ("LoadState,ActiveState,SubState,UnitFileState,NRestarts,"
             "ActiveEnterTimestampMonotonic,WorkingDirectory,Environment")
    intent = runtime_state.get_service_settings(key)
    try:
        r = _run(["systemctl", "show", unit, "--property=" + props])
    except Exception as e:  # systemctl 不存在(如开发机) → 视为未安装
        return {"installed": False, "active_state": "unknown", "sub_state": "",
                "enabled": False, "uptime_seconds": None, "n_restarts": None,
                "bound_app": None, "bound_config": None, "working_dir": None,
                "path_ok": False, **intent, "error": str(e)}

    kv: Dict[str, str] = {}
    for line in r.stdout.splitlines():
        k, _, v = line.partition("=")
        kv[k] = v

    installed = kv.get("LoadState", "") == "loaded"
    active_state = kv.get("ActiveState", "inactive")

    # uptime: 用 ActiveEnterTimestampMonotonic(自启动起 µs) 对 /proc/uptime, 避开时区
    uptime = None
    try:
        mono_s = int(kv.get("ActiveEnterTimestampMonotonic", "0")) / 1_000_000.0
        if active_state == "active" and mono_s > 0:
            with open("/proc/uptime") as f:
                boot_s = float(f.read().split()[0])
            uptime = max(0, int(boot_s - mono_s))
    except Exception:
        uptime = None

    try:
        n_restarts = int(kv.get("NRestarts", ""))
    except ValueError:
        n_restarts = None

    # WorkingDirectory: 既反推绑定到哪个 App, 也判断路径是否真实存在。
    # path_ok=False 即「失效单元」(如残留旧单元指向已删目录) → 面板会改走「重新安装」强制修正。
    wd = kv.get("WorkingDirectory", "")
    bound_app = None
    path_ok = False
    if installed:
        path_ok = bool(wd) and os.path.isdir(wd)
        try:
            if wd and str(Path(wd)).startswith(str(APPS_ROOT)):
                bound_app = Path(wd).relative_to(APPS_ROOT).parts[0]
        except Exception:
            bound_app = None

    return {
        "installed": installed,
        "active_state": active_state,        # active / inactive / failed / activating / unknown
        "sub_state": kv.get("SubState", ""),
        "enabled": kv.get("UnitFileState", "") in ("enabled", "enabled-runtime"),
        "uptime_seconds": uptime,
        "n_restarts": n_restarts,
        "bound_app": bound_app,
        "bound_config": _environment_value(kv.get("Environment", ""), "CONFIG_FILE"),
        "working_dir": wd or None,
        "path_ok": path_ok,
        **intent,
    }


@router.get("/services")
async def list_services():
    out = []
    for key, meta in MANAGED.items():
        out.append({"key": key, "label": meta["label"], "unit": meta["unit"], **_status(key)})
    return out


class AutostartReq(BaseModel):
    enabled: bool


def _running_context_or_409() -> Dict[str, Any]:
    context = pm.get_running_app_context()
    if context is None:
        raise HTTPException(status_code=409, detail="没有正在运行的视觉程序，请先启动视觉程序")
    return context


def _write_and_start_unlocked(key: str, context: Dict[str, Any]) -> Dict[str, Any]:
    """调用方必须持有 pm.runtime_lock，保证查找和绑定之间视觉 App 不会切换。"""
    meta = _svc(key)
    app_name = str(context["app"])
    app_dir = Path(context["app_dir"]).resolve()
    config_name = str(context.get("config") or "config.json")

    # 确保持久数据目录存在并已迁移初始文件
    migrate_app_data(app_name, app_dir)
    try:
        app_dir.relative_to(APPS_ROOT.resolve())
    except ValueError:
        raise HTTPException(status_code=400, detail=f"视觉程序目录不在 APPS_ROOT 下：{app_dir}")
    subdir = app_dir / meta["subdir"]
    if not subdir.is_dir():
        raise HTTPException(
            status_code=400,
            detail=f"{app_name} 下没有 {meta['subdir']}（该 App 未打包此服务）",
        )

    unit = meta["unit"]
    try:
        (SYSTEMD_DIR / unit).write_text(
            _unit_content(key, app_dir, config_name), encoding="utf-8"
        )
        reloaded = _run(["systemctl", "daemon-reload"])
        if reloaded.returncode != 0:
            raise HTTPException(status_code=500, detail=f"daemon-reload 失败: {reloaded.stderr.strip()}")
        # 开机恢复由 Web 控制台统一编排，避免 unit 在视觉程序之前按旧路径独立启动。
        _run(["systemctl", "disable", unit])
        _run(["systemctl", "reset-failed", unit])
        started = _run(["systemctl", "restart", unit])
    except PermissionError:
        raise HTTPException(status_code=500, detail="写入 systemd 单元失败：控制台需以 root 运行")
    except HTTPException:
        raise
    except Exception as exc:
        raise HTTPException(status_code=500, detail=str(exc))
    if started.returncode != 0:
        raise HTTPException(
            status_code=500,
            detail="单元已绑定当前视觉程序，但启动失败：" +
                   (started.stderr or started.stdout or "").strip(),
        )
    runtime_state.mark_service_started(key)
    return {
        "ok": True,
        "unit": unit,
        "app": app_name,
        "config": config_name if key == "ota_agent" else None,
        "started": True,
    }


def start_for_running_app(key: str) -> Dict[str, Any]:
    _svc(key)
    with pm.runtime_lock():
        return _write_and_start_unlocked(key, _running_context_or_409())


def sync_services_for_running_app() -> Dict[str, List[str]]:
    """视觉程序启动/切换后，迁移运行中的服务并恢复等待自启的服务。"""
    updated: List[str] = []
    errors: List[str] = []
    with pm.runtime_lock():
        context = pm.get_running_app_context()
        if context is None:
            return {"updated": updated, "errors": ["视觉程序启动后未能读取运行上下文"]}
        for key in MANAGED:
            status = _status(key)
            active = status.get("active_state") in ("active", "activating", "reloading")
            waiting_autostart = bool(status.get("autostart") and status.get("desired_running"))
            ota_config_matches = (
                key != "ota_agent" or status.get("bound_config") == context.get("config")
            )
            binding_matches = (
                status.get("path_ok") and status.get("bound_app") == context.get("app")
                and ota_config_matches
            )
            if active and binding_matches:
                # Web 统一编排自启，避免 systemd 绕过视觉 App 的绑定关系。
                _run(["systemctl", "disable", MANAGED[key]["unit"]])
                continue
            if not active and not waiting_autostart:
                continue
            try:
                _write_and_start_unlocked(key, context)
                updated.append(key)
            except HTTPException as exc:
                errors.append(f"{MANAGED[key]['label']}：{exc.detail}")
            except Exception as exc:
                errors.append(f"{MANAGED[key]['label']}：{exc}")
    return {"updated": updated, "errors": errors}


@router.post("/services/{key}/autostart")
async def set_service_autostart(key: str, req: AutostartReq):
    _svc(key)
    with pm.runtime_lock():
        settings = runtime_state.set_service_autostart(key, req.enabled)
        # Web 使用统一编排恢复，不能让 systemd 绕过视觉 App 匹配独立拉起旧 unit。
        _run(["systemctl", "disable", MANAGED[key]["unit"]])
    return {"ok": True, **settings}


@router.post("/services/{key}/{action}")
async def control_service(key: str, action: str):
    _svc(key)
    if action not in ("start", "stop", "restart"):
        raise HTTPException(status_code=400, detail="action 仅支持 start/stop/restart")
    if action in ("start", "restart"):
        return start_for_running_app(key)

    unit = MANAGED[key]["unit"]
    with pm.runtime_lock():
        r = _run(["systemctl", "stop", unit])
        if r.returncode != 0:
            raise HTTPException(status_code=500, detail=(r.stderr or r.stdout or "systemctl 失败").strip())
        runtime_state.mark_service_stopped(key)
    return {"ok": True}


@router.get("/services/{key}/logs")
async def service_logs(key: str, lines: int = 200):
    _svc(key)
    unit = MANAGED[key]["unit"]
    lines = max(1, min(int(lines), 1000))
    try:
        r = _run(["journalctl", "-u", unit, "-n", str(lines), "--no-pager", "--output=short-iso"],
                 timeout=20)
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))
    return {"lines": r.stdout.splitlines()}
