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
import subprocess
from pathlib import Path
from typing import Any, Dict, List

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel

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


def _unit_content(key: str, app_dir: Path) -> str:
    """生成与 deploy.sh 等价的单元内容(同名同义), 路径指向所选 App 的 services/ 子目录。"""
    if key == "ota_agent":
        return (
            "[Unit]\n"
            "Description=Edge Box OTA Agent\n"
            "After=network.target\n\n"
            "[Service]\n"
            "Type=simple\n"
            f"WorkingDirectory={app_dir}/services/model_update\n"
            f"Environment=ASSETS_DIR={app_dir}/assets\n"
            f"ExecStart=/usr/bin/python3 -u {app_dir}/services/model_update/ota_agent.py\n"
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
        "Wants=network-online.target\n\n"
        "[Service]\n"
        "Type=simple\n"
        f"WorkingDirectory={app_dir}/services/upload\n"
        "ExecStart=/usr/bin/python3 -u main.py\n"
        "Restart=always\n"
        "RestartSec=5\n"
        "User=root\n\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n"
    )


def _status(key: str) -> Dict[str, Any]:
    unit = MANAGED[key]["unit"]
    props = ("LoadState,ActiveState,SubState,UnitFileState,NRestarts,"
             "ActiveEnterTimestampMonotonic,WorkingDirectory")
    try:
        r = _run(["systemctl", "show", unit, "--property=" + props])
    except Exception as e:  # systemctl 不存在(如开发机) → 视为未安装
        return {"installed": False, "active_state": "unknown", "sub_state": "",
                "enabled": False, "uptime_seconds": None, "n_restarts": None,
                "bound_app": None, "working_dir": None, "path_ok": False, "error": str(e)}

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
        "working_dir": wd or None,
        "path_ok": path_ok,
    }


@router.get("/services")
async def list_services():
    out = []
    for key, meta in MANAGED.items():
        out.append({"key": key, "label": meta["label"], "unit": meta["unit"], **_status(key)})
    return out


class InstallReq(BaseModel):
    app: str


@router.post("/services/{key}/install")
async def install_service(key: str, req: InstallReq):
    _svc(key)
    app_dir = (APPS_ROOT / req.app).resolve()
    if not str(app_dir).startswith(str(APPS_ROOT.resolve())) or not app_dir.is_dir():
        raise HTTPException(status_code=404, detail=f"App 不存在: {req.app}")
    subdir = app_dir / MANAGED[key]["subdir"]
    if not subdir.is_dir():
        raise HTTPException(
            status_code=400,
            detail=f"{req.app} 下没有 {MANAGED[key]['subdir']}（该 App 未打包此服务）",
        )

    unit = MANAGED[key]["unit"]
    try:
        # 强制覆盖单元文件 → WorkingDirectory/ExecStart 指向当前 App 的 services/
        # （任何旧路径/失效单元，包括 deploy.sh 装的残留单元，都被直接改成程序包里的路径）
        (SYSTEMD_DIR / unit).write_text(_unit_content(key, app_dir), encoding="utf-8")
        rl = _run(["systemctl", "daemon-reload"])
        if rl.returncode != 0:
            raise HTTPException(status_code=500, detail=f"daemon-reload 失败: {rl.stderr.strip()}")
        _run(["systemctl", "enable", unit])          # 开机自启
        _run(["systemctl", "reset-failed", unit])    # 清掉旧单元/失败留下的 failed 终态
        st = _run(["systemctl", "restart", unit])    # 用新单元直接(重)启动 —— 默认装完即运行
    except PermissionError:
        raise HTTPException(status_code=500, detail="写入 systemd 单元失败：控制台需以 root 运行")
    except HTTPException:
        raise
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))
    if st.returncode != 0:
        raise HTTPException(status_code=500,
                            detail="单元已写入并指向正确路径，但启动失败：" + (st.stderr or st.stdout or "").strip())
    return {"ok": True, "unit": unit, "app": req.app, "started": True}


@router.post("/services/{key}/{action}")
async def control_service(key: str, action: str):
    _svc(key)
    if action not in ("start", "stop", "restart"):
        raise HTTPException(status_code=400, detail="action 仅支持 start/stop/restart")
    unit = MANAGED[key]["unit"]
    if action in ("start", "restart"):
        # 防御：单元工作目录失效(残留旧单元指向已删目录)→ 给明确提示，而不是底层 CHDIR 错
        wd = _run(["systemctl", "show", unit, "--property=WorkingDirectory"]).stdout.partition("=")[2].strip()
        if wd and not os.path.isdir(wd):
            raise HTTPException(status_code=409,
                detail=f"单元工作目录不存在：{wd}。该单元路径已失效，请在面板「重新安装」到当前程序包。")
        # 清掉 failed 终态(systemd 放弃重试后), 否则可能拉不起来
        _run(["systemctl", "reset-failed", unit])
    r = _run(["systemctl", action, unit])
    if r.returncode != 0:
        raise HTTPException(status_code=500, detail=(r.stderr or r.stdout or "systemctl 失败").strip())
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
