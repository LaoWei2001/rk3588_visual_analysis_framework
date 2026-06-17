"""
process_manager.py — App 进程生命周期管理

变更说明：
  - 日志不再写入任何文件（run.log 已移除）；
    subprocess.PIPE 捕获 stdout+stderr，由后台线程推入内存缓冲（log_buffer）。
  - config.json / roi_zones.json 统一位于 assets/ 子目录；
    启动时把 assets/config.json 作为命令行参数传给二进制。
"""

import json
import os
import signal
import subprocess
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Optional

APPS_ROOT   = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))
BINARY_NAME = os.environ.get("BINARY_NAME", "rk3588_yolo")


@dataclass
class ManagedProcess:
    app_name:   str
    pid:        int
    mode:       str
    started_at: float
    proc:       subprocess.Popen   # None when recovered (no pipe)
    config:     str = "config.json"  # 本次启动所用的配置文件名（assets/ 下）


_processes: Dict[str, ManagedProcess] = {}


def _app_path(app_name: str) -> Path:
    return APPS_ROOT / app_name


def _normalize_config_name(config_name: Optional[str]) -> str:
    """把启动请求里的配置文件名归一化为 assets/ 下的纯文件名。

    接受 'config.json' / 'assets/config.json'；禁止路径穿越，必须是 .json。
    """
    name = (config_name or "config.json").strip().replace("\\", "/")
    if name.startswith("assets/"):
        name = name[len("assets/"):]
    if not name:
        name = "config.json"
    if "/" in name or not name.endswith(".json"):
        raise ValueError(f"非法配置文件名: {config_name}")
    return name


def _patch_display(config_path: Path, enable: bool) -> None:
    """原子修改 config.json 中 global.enable_display 字段。

    deploy 模式启动时调用（enable=False），确保推理程序不尝试输出 HDMI，
    即使用户在编辑器里误勾选了「HDMI 显示」也不会影响无显示器环境。
    """
    try:
        cfg = json.loads(config_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return  # 读取失败不阻断启动

    target = 1 if enable else 0

    # 兼容两种 schema：顶层 global 对象 / 或字段直接在根
    changed = False
    if isinstance(cfg.get("global"), dict):
        if cfg["global"].get("enable_display") != target:
            cfg["global"]["enable_display"] = target
            changed = True
    elif "enable_display" in cfg:
        if cfg["enable_display"] != target:
            cfg["enable_display"] = target
            changed = True

    if not changed:
        return

    # 原子写：先写 .tmp 再 replace，防止写到一半崩溃损坏配置
    tmp = config_path.with_suffix(".tmp")
    try:
        tmp.write_text(json.dumps(cfg, ensure_ascii=False, indent=4), encoding="utf-8")
        os.replace(tmp, config_path)
    except OSError:
        tmp.unlink(missing_ok=True)  # 清理残留临时文件，不阻断启动


# ── 启动时恢复上次未退出的进程 ───────────────────────────────────────────────

def recover_processes() -> None:
    """Web 服务启动时，重新关联上次仍在运行的推理进程（无法恢复日志管道）。"""
    if not APPS_ROOT.exists():
        return
    for entry in APPS_ROOT.iterdir():
        if not entry.is_dir() or entry.name.startswith("_"):
            continue
        pid_file  = entry / "run.pid"
        mode_file = entry / "run.mode"
        cfg_file  = entry / "run.config"
        if not pid_file.exists():
            continue
        try:
            pid  = int(pid_file.read_text().strip())
            os.kill(pid, 0)                         # 进程存活检查
            mode = mode_file.read_text().strip() if mode_file.exists() else "deploy"
            cfg  = cfg_file.read_text().strip() if cfg_file.exists() else "config.json"
            _processes[entry.name] = ManagedProcess(
                app_name=entry.name, pid=pid, mode=mode,
                started_at=time.time(), proc=None,  # type: ignore[arg-type]
                config=cfg,
            )
            # 在内存缓冲写一条提示，提醒用户日志从此处续接
            from services.log_buffer import get_log_buffer
            get_log_buffer(entry.name).push(
                f"[控制台已重启，进程 PID={pid} 仍在运行；本次会话日志从此处续接]"
            )
        except (ValueError, ProcessLookupError, PermissionError):
            pid_file.unlink(missing_ok=True)


# ── 启动 ─────────────────────────────────────────────────────────────────────

def _discover_xauthority() -> Optional[str]:
    """找到当前 X 服务器(:0)的鉴权 cookie 文件，供无图形会话的后台进程连显示用；找不到返回 None。"""
    # 1) 最准：从正在运行的 Xorg / X 进程命令行里取 `-auth <file>`
    try:
        pids = subprocess.run(["pgrep", "-x", "Xorg"], capture_output=True, text=True, timeout=3).stdout.split()
        if not pids:
            pids = subprocess.run(["pgrep", "-x", "X"], capture_output=True, text=True, timeout=3).stdout.split()
        for pid in pids:
            try:
                toks = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\x00")
                for i, tok in enumerate(toks):
                    if tok == b"-auth" and i + 1 < len(toks):
                        p = toks[i + 1].decode("utf-8", "ignore")
                        if p and os.path.exists(p):
                            return p
            except OSError:
                continue
    except Exception:
        pass
    # 2) 兜底：常见 cookie 路径
    candidates = ["/run/lightdm/root/:0", "/var/run/lightdm/root/:0",
                  "/run/user/0/gdm/Xauthority", "/root/.Xauthority"]
    try:
        candidates += [str(p) for p in sorted(Path("/home").glob("*/.Xauthority"))]
    except OSError:
        pass
    for p in candidates:
        if os.path.exists(p):
            return p
    return None


def _setup_display_env(env: dict) -> None:
    """让后台(systemd, 无图形会话)拉起的程序也能在板端 HDMI 上显示。

    根因：rk3588_yolo 的 GTK 显示靠继承环境里的 DISPLAY+XAUTHORITY 连 :0；命令行启动能从已登录会话
    继承到这些，而本服务(User=root, multi-user.target)没有 → 冷启动连不上 X，表现为“要先在命令行手动
    跑一次才显示”。这里补齐 DISPLAY / XAUTHORITY 并尽力放行本地 root。全程 best-effort，失败即退回原行为。
    """
    env.setdefault("DISPLAY", ":0")
    if not (env.get("XAUTHORITY") and os.path.exists(env["XAUTHORITY"])):
        xauth = _discover_xauthority()
        if xauth:
            env["XAUTHORITY"] = xauth
    # 即便 cookie 不对，也尽力让 X 放行本地 root 客户端（覆盖“X 访问控制开着”的情况）
    try:
        subprocess.run(["xhost", "+SI:localuser:root"], env=env, capture_output=True, timeout=3)
    except Exception:
        pass


def start_app(app_name: str, mode: str, config_name: Optional[str] = None) -> int:
    app_dir     = _app_path(app_name)
    binary      = app_dir / BINARY_NAME
    assets_dir  = app_dir / "assets"
    config_name = _normalize_config_name(config_name)
    config      = assets_dir / config_name

    if not binary.exists():
        raise FileNotFoundError(f"找不到可执行文件: {binary}")
    if not config.exists():
        raise FileNotFoundError(f"找不到配置文件: {config}（请先在编辑器中保存配置）")

    # 确保二进制有执行权限（从 Windows 复制过来时常见问题）
    if not os.access(binary, os.X_OK):
        os.chmod(binary, 0o755)

    stop_app(app_name)

    # 根据启动模式自动同步 enable_display：部署=0，调试=1
    _patch_display(config, enable=(mode == "debug"))

    # 清空内存日志缓冲，准备新一轮输出
    from services.log_buffer import get_log_buffer
    buf = get_log_buffer(app_name)
    buf.clear()

    env = os.environ.copy()
    # Debug 模式要在板端 HDMI 上显示：补齐 X 显示环境（DISPLAY + XAUTHORITY + 放行本地 root）。
    # 否则 systemd 服务(无图形会话)拉起的程序连不上 X，表现为“先得在命令行手动跑一次才显示”。
    if mode == "debug":
        _setup_display_env(env)

    # 用 PIPE 捕获 stdout+stderr，不落盘
    proc = subprocess.Popen(
        [str(binary), str(config)],
        cwd=str(app_dir),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,    # Python 侧行缓冲；C 侧因走管道是块缓冲，但不影响最终正确性
        env=env,
    )

    # 后台 reader 线程：从管道逐行读取，推入内存缓冲
    def _pipe_reader() -> None:
        try:
            for line in proc.stdout:            # type: ignore[union-attr]
                buf.push(line.rstrip("\n"))
        except Exception:
            pass
        buf.push("[进程已停止]")

    threading.Thread(target=_pipe_reader, daemon=True, name=f"log-reader-{app_name}").start()

    _processes[app_name] = ManagedProcess(
        app_name=app_name, pid=proc.pid, mode=mode,
        started_at=time.time(), proc=proc, config=config_name,
    )

    # 持久化 PID / 模式 / 配置文件名，供服务重启后恢复与展示
    (app_dir / "run.pid").write_text(str(proc.pid))
    (app_dir / "run.mode").write_text(mode)
    (app_dir / "run.config").write_text(config_name)

    return proc.pid


# ── 停止 ─────────────────────────────────────────────────────────────────────

def stop_app(app_name: str) -> bool:
    mp = _processes.get(app_name)
    if not mp:
        # 没有 Popen 句柄时，尝试通过 PID 文件杀进程
        pid_file = _app_path(app_name) / "run.pid"
        if pid_file.exists():
            try:
                pid = int(pid_file.read_text().strip())
                os.kill(pid, signal.SIGTERM)
                time.sleep(0.5)
                try:
                    os.kill(pid, 0)
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            except (ValueError, ProcessLookupError):
                pass
            pid_file.unlink(missing_ok=True)
        return False

    try:
        if mp.proc is not None:
            mp.proc.send_signal(signal.SIGTERM)
            try:
                mp.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                mp.proc.kill()
                mp.proc.wait()
        else:
            os.kill(mp.pid, signal.SIGTERM)
            time.sleep(5)
            try:
                os.kill(mp.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    except ProcessLookupError:
        pass
    finally:
        _processes.pop(app_name, None)
        (_app_path(app_name) / "run.pid").unlink(missing_ok=True)

    return True


# ── 状态查询 ─────────────────────────────────────────────────────────────────

def get_status(app_name: str) -> dict:
    stopped = {"status": "stopped", "mode": None, "pid": None,
               "uptime_seconds": None, "config": None}

    mp = _processes.get(app_name)
    if not mp:
        return stopped

    if mp.proc is not None:
        if mp.proc.poll() is not None:
            _processes.pop(app_name, None)
            (_app_path(app_name) / "run.pid").unlink(missing_ok=True)
            return stopped
    else:
        try:
            os.kill(mp.pid, 0)
        except ProcessLookupError:
            _processes.pop(app_name, None)
            (_app_path(app_name) / "run.pid").unlink(missing_ok=True)
            return stopped

    return {
        "status":         "running",
        "mode":           mp.mode,
        "pid":            mp.pid,
        "uptime_seconds": int(time.time() - mp.started_at),
        "config":         mp.config,
    }
