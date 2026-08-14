"""
process_manager.py — App 进程生命周期管理

变更说明：
  - 日志不再写入任何文件（run.log 已移除）；
    systemd-run --pipe 捕获 stdout+stderr，由后台线程推入内存缓冲（log_buffer）。
  - 运行配置统一位于 assets/ 子目录，ROI 保存在 channels[].roi_zones；
    启动时把 assets/config.json 作为命令行参数传给二进制。
"""

import fcntl
import hashlib
import json
import os
import re
import signal
import subprocess
import threading
import time
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterator, Optional

from services import runtime_state
from services.data_dir import data_dir
from services import storage_manager

APPS_ROOT   = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))
BINARY_NAME = os.environ.get("BINARY_NAME", "vision_analysis")


@dataclass
class ManagedProcess:
    app_name:   str
    pid:        int
    mode:       str
    started_at: float
    proc:       Optional[subprocess.Popen]  # None when recovered or managed by systemd
    config:     str = "config.json"  # 本次启动所用的配置文件名（assets/ 下）
    unit_name:  Optional[str] = None
    launcher:   Optional[subprocess.Popen] = None  # systemd-run --pipe 日志代理


_processes: Dict[str, ManagedProcess] = {}
_start_thread_lock = threading.Lock()
_start_lock_path = APPS_ROOT / ".app_start.lock"


class AppAlreadyRunningError(RuntimeError):
    """尝试启动第二个 App 时抛出，由 API 转换成 HTTP 409。"""

    def __init__(self, app_name: str, pid: Optional[int]):
        self.app_name = app_name
        self.pid = pid
        pid_text = f"（PID {pid}）" if pid is not None else ""
        super().__init__(f"程序 {app_name}{pid_text} 正在运行，请先停止它再启动其他程序")


@contextmanager
def _exclusive_start_lock() -> Iterator[None]:
    """跨线程、跨 worker 串行化整个运行组合的变更。"""
    APPS_ROOT.mkdir(parents=True, exist_ok=True)
    with _start_thread_lock:
        with _start_lock_path.open("a+") as lock_file:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)


@contextmanager
def runtime_lock() -> Iterator[None]:
    """供后台服务绑定流程复用的全局运行锁。

    服务必须在“查找当前视觉 App → 重写 unit → 启动”的整个过程中持有此锁，
    避免视觉 App 恰好切换而绑定到旧目录。
    """
    with _exclusive_start_lock():
        yield


def _app_path(app_name: str) -> Path:
    return APPS_ROOT / app_name


def _app_unit_name(app_name: str) -> str:
    slug = re.sub(r"[^A-Za-z0-9_.-]+", "-", app_name).strip(".-")[:40] or "app"
    digest = hashlib.sha256(app_name.encode("utf-8")).hexdigest()[:12]
    return f"rk3588-app-{slug}-{digest}.service"


def _read_unit_name(app_name: str) -> Optional[str]:
    try:
        unit_name = (_app_path(app_name) / "run.systemd_unit").read_text().strip()
    except OSError:
        return None
    expected = _app_unit_name(app_name)
    return unit_name if unit_name == expected else None


def _read_pid(app_name: str) -> Optional[int]:
    try:
        return int((_app_path(app_name) / "run.pid").read_text().strip())
    except (OSError, ValueError):
        return None


def _current_boot_id() -> str:
    try:
        return Path("/proc/sys/kernel/random/boot_id").read_text().strip()
    except OSError:
        return ""


def _clear_stale_runtime_marker(app_name: str) -> None:
    app_dir = _app_path(app_name)
    (app_dir / "run.pid").unlink(missing_ok=True)
    (app_dir / "run.control.sock").unlink(missing_ok=True)
    (app_dir / "run.boot_id").unlink(missing_ok=True)
    (app_dir / "run.systemd_unit").unlink(missing_ok=True)


def _pid_belongs_to_app(app_name: str, pid: int) -> bool:
    """验证 PID 确实是该 App 的视觉进程，避免重启后 PID 被其他进程复用。"""
    app_dir = _app_path(app_name)
    boot_file = app_dir / "run.boot_id"
    current_boot = _current_boot_id()
    if boot_file.exists() and current_boot:
        try:
            if boot_file.read_text().strip() != current_boot:
                return False
        except OSError:
            return False

    try:
        expected_exe = (app_dir / BINARY_NAME).resolve(strict=True)
        actual_exe = Path(f"/proc/{pid}/exe").resolve(strict=True)
        actual_cwd = Path(f"/proc/{pid}/cwd").resolve(strict=True)
        return actual_exe == expected_exe and actual_cwd == app_dir.resolve(strict=True)
    except OSError:
        return False


def _find_running_app(exclude: Optional[str] = None) -> Optional[ManagedProcess]:
    """扫描磁盘运行标记，返回一个仍存活的 App；用于全局单实例启动约束。"""
    if not APPS_ROOT.exists():
        return None
    try:
        entries = list(APPS_ROOT.iterdir())
    except OSError:
        return None
    for entry in sorted(entries, key=lambda item: item.name):
        if not entry.is_dir() or entry.name.startswith((".", "_")) or entry.name == exclude:
            continue
        status = get_status(entry.name)
        if status.get("status") == "running":
            return _processes.get(entry.name) or _recover_process(entry.name)
    return None


def _recover_process(app_name: str, announce: bool = False) -> Optional[ManagedProcess]:
    """从App目录的运行标记恢复一个进程，供多worker和跨请求状态查询使用。"""
    app_dir = _app_path(app_name)
    pid_file = app_dir / "run.pid"
    pid = _read_pid(app_name)
    if pid is None:
        if pid_file.exists():
            _clear_stale_runtime_marker(app_name)
        return None

    try:
        os.kill(pid, 0)
    except (ProcessLookupError, PermissionError):
        _clear_stale_runtime_marker(app_name)
        return None
    if not _pid_belongs_to_app(app_name, pid):
        _clear_stale_runtime_marker(app_name)
        return None

    mode_file = app_dir / "run.mode"
    cfg_file = app_dir / "run.config"
    started_file = app_dir / "run.started_at"
    try:
        mode = mode_file.read_text().strip() if mode_file.exists() else "deploy"
    except OSError:
        mode = "deploy"
    try:
        config = cfg_file.read_text().strip() if cfg_file.exists() else "config.json"
    except OSError:
        config = "config.json"
    try:
        started_at = float(started_file.read_text().strip())
        if started_at <= 0 or started_at > time.time():
            raise ValueError
    except (OSError, ValueError):
        started_at = time.time()

    managed = ManagedProcess(
        app_name=app_name,
        pid=pid,
        mode=mode or "deploy",
        started_at=started_at,
        proc=None,
        config=config or "config.json",
        unit_name=_read_unit_name(app_name),
    )
    _processes[app_name] = managed

    if announce:
        from services.log_buffer import get_log_buffer
        get_log_buffer(app_name).push(
            f"[控制台已重新关联运行进程 PID={pid}；本次会话日志从此处续接]"
        )
    return managed


def _clear_runtime_files_if_pid(app_name: str, pid: int) -> None:
    """只清理仍属于指定PID的标记，避免其他worker刚启动的新进程标记被误删。"""
    if _read_pid(app_name) != pid:
        return
    app_dir = _app_path(app_name)
    (app_dir / "run.pid").unlink(missing_ok=True)
    (app_dir / "run.control.sock").unlink(missing_ok=True)
    (app_dir / "run.boot_id").unlink(missing_ok=True)
    (app_dir / "run.systemd_unit").unlink(missing_ok=True)


def get_running_app_context() -> Optional[dict]:
    """返回当前唯一视觉程序的 App/配置上下文；需要强一致时由调用方持有 runtime_lock。"""
    running = _find_running_app()
    if running is None:
        return None
    status = get_status(running.app_name)
    if status.get("status") != "running":
        return None
    return {
        "app": running.app_name,
        "app_dir": _app_path(running.app_name),
        "pid": running.pid,
        "mode": status.get("mode") or "deploy",
        "config": status.get("config") or "config.json",
    }


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

    global_config = cfg.get("global")
    if not isinstance(global_config, dict):
        return
    changed = global_config.get("enable_display") != target
    if changed:
        global_config["enable_display"] = target

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
        if not (entry / "run.pid").exists():
            continue
        _recover_process(entry.name, announce=True)


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

    根因：vision_analysis 的 GTK 显示靠继承环境里的 DISPLAY+XAUTHORITY 连 :0；命令行启动能从已登录会话
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


def _systemd_main_pid(unit_name: str) -> Optional[int]:
    try:
        result = subprocess.run(
            ["systemctl", "show", unit_name, "-p", "MainPID", "--value"],
            capture_output=True,
            text=True,
            timeout=5,
        )
        if result.returncode != 0:
            return None
        pid = int(result.stdout.strip())
        return pid if pid > 0 else None
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return None


def _wait_for_systemd_main_pid(
    unit_name: str,
    launcher: subprocess.Popen,
    timeout: float = 10.0,
) -> Optional[int]:
    """等待 transient service 进入运行态，但不等待视觉程序退出。

    ``systemd-run --pipe`` 会在服务整个生命周期内保持运行，用来把 stdout/stderr
    转发到控制台的内存日志缓冲。不能对 launcher 调用 ``wait(timeout=...)`` 来
    判断“启动完成”，否则所有正常的长驻视觉程序都会被误判为启动超时。
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if launcher.poll() is not None:
            return None
        pid = _systemd_main_pid(unit_name)
        if pid is not None:
            return pid
        time.sleep(0.1)
    return None


def _list_running_app_units() -> list[str]:
    """返回所有仍有 MainPID 的 Web 托管视觉 transient service。"""
    try:
        result = subprocess.run(
            [
                "systemctl", "list-units", "--type=service", "--all",
                "--plain", "--no-legend", "rk3588-app-*.service",
            ],
            capture_output=True,
            text=True,
            timeout=5,
        )
    except (OSError, subprocess.TimeoutExpired):
        return []
    if result.returncode != 0:
        return []

    units = []
    for line in result.stdout.splitlines():
        fields = line.split()
        if not fields:
            continue
        unit_name = fields[0]
        if not re.fullmatch(r"rk3588-app-[A-Za-z0-9_.-]+\.service", unit_name):
            continue
        if _systemd_main_pid(unit_name) is not None:
            units.append(unit_name)
    return sorted(set(units))


def _stop_systemd_unit(unit_name: str) -> bool:
    try:
        result = subprocess.run(
            ["systemctl", "stop", unit_name],
            capture_output=True,
            text=True,
            timeout=15,
        )
        return result.returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def _terminate_pid(pid: int) -> None:
    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        return

    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return
        time.sleep(0.2)

    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def _start_systemd_app(binary: Path, config: Path, app_dir: Path, env: dict, unit_name: str) -> subprocess.Popen:
    command = [
        "systemd-run",
        f"--unit={unit_name}",
        "--collect",
        "--quiet",
        "--pipe",
        "--service-type=exec",
        "--property=KillMode=control-group",
        "--property=TimeoutStopSec=10",
        f"--working-directory={app_dir}",
    ]
    for key, value in sorted(env.items()):
        if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key):
            command.append(f"--setenv={key}={value}")
    command.extend([str(binary), str(config)])
    return subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )


def start_app(app_name: str, mode: str, config_name: Optional[str] = None) -> int:
    app_dir     = _app_path(app_name)
    binary      = app_dir / BINARY_NAME
    assets_dir  = app_dir / "assets"
    control_sock = app_dir / "run.control.sock"
    config_name = _normalize_config_name(config_name)
    config      = assets_dir / config_name

    if not binary.exists():
        raise FileNotFoundError(f"找不到可执行文件: {binary}")
    if not config.exists():
        raise FileNotFoundError(f"找不到配置文件: {config}（请先在编辑器中保存配置）")

    # 确保二进制有执行权限（从 Windows 复制过来时常见问题）
    if not os.access(binary, os.X_OK):
        os.chmod(binary, 0o755)

    # 检查其他 App、重启同名 App、拉起进程和写入 PID 必须处于同一把跨 worker 锁内。
    # 否则两个浏览器同时点击不同 App 时，都可能在对方写 PID 前通过检查。
    with _exclusive_start_lock():
        running = _find_running_app(exclude=app_name)
        if running is not None:
            raise AppAlreadyRunningError(running.app_name, running.pid)

        # 同一个 App 再次启动仍保持原来的“重启”语义，不会与其他 App 并存。
        _stop_app_unlocked(app_name)

        # 根据启动模式自动同步 enable_display：部署=0，调试=1
        _patch_display(config, enable=(mode == "debug"))

        # 清空内存日志缓冲，准备新一轮输出
        from services.log_buffer import get_log_buffer
        buf = get_log_buffer(app_name)
        buf.clear()

        env = os.environ.copy()
        control_sock.unlink(missing_ok=True)
        env["RK_CHANNEL_CONTROL_SOCKET"] = str(control_sock)
        env["ASSETS_DIR"] = str(assets_dir)
        env["EVENT_STORE_DIR"] = str(data_dir(app_name) / "event_store")
        env.update(storage_manager.vision_environment())
        bundled_libs = str(app_dir / "libs")
        existing_ld_path = env.get("LD_LIBRARY_PATH", "")
        env["LD_LIBRARY_PATH"] = (
            f"{bundled_libs}:{existing_ld_path}" if existing_ld_path else bundled_libs
        )
        # Debug 模式要在板端 HDMI 上显示：补齐 X 显示环境（DISPLAY + XAUTHORITY + 放行本地 root）。
        # 否则 systemd 服务(无图形会话)拉起的程序连不上 X，表现为“先得在命令行手动跑一次才显示”。
        if mode == "debug":
            _setup_display_env(env)

        unit_name = _app_unit_name(app_name)
        # run.pid 可能因程序目录被外部覆盖而丢失；固定名称的旧 transient unit
        # 仍可能存活。启动前必须先清掉，否则 systemd-run 会因同名 unit 已存在而
        # 失败，而状态查询又可能误取旧 unit 的 MainPID。
        if _systemd_main_pid(unit_name) is not None:
            if not _stop_systemd_unit(unit_name):
                raise RuntimeError(f"无法停止同名残留视觉进程服务: {unit_name}")

        try:
            launcher = _start_systemd_app(binary, config, app_dir, env, unit_name)
        except FileNotFoundError as exc:
            raise RuntimeError("找不到 systemd-run，无法启动独立视觉进程服务") from exc

        def _pipe_reader() -> None:
            try:
                for line in launcher.stdout:        # type: ignore[union-attr]
                    buf.push(line.rstrip("\n"))
            except Exception:
                pass
            try:
                launcher.wait(timeout=1)
            except (subprocess.TimeoutExpired, ProcessLookupError):
                pass
            buf.push("[进程已停止]")

        threading.Thread(target=_pipe_reader, daemon=True, name=f"log-reader-{app_name}").start()

        pid = _wait_for_systemd_main_pid(unit_name, launcher)
        if pid is None:
            launcher.poll()
            _stop_systemd_unit(unit_name)
            if launcher.poll() is None:
                launcher.terminate()
            if launcher.returncode not in (None, 0):
                raise RuntimeError("创建视觉进程服务失败")
            raise RuntimeError("视觉进程服务未能启动")
        if not _pid_belongs_to_app(app_name, pid):
            _stop_systemd_unit(unit_name)
            raise RuntimeError("视觉进程已启动，但其可执行文件或工作目录与当前程序包不一致")

        started_at = time.time()
        _processes[app_name] = ManagedProcess(
            app_name=app_name, pid=pid, mode=mode,
            started_at=started_at, proc=None, config=config_name,
            unit_name=unit_name, launcher=launcher,
        )

        try:
            # PID文件是运行状态的提交标记，最后写入；其他worker看到PID时，其余元数据已经完整。
            (app_dir / "run.mode").write_text(mode)
            (app_dir / "run.config").write_text(config_name)
            (app_dir / "run.started_at").write_text(str(started_at))
            (app_dir / "run.boot_id").write_text(_current_boot_id())
            (app_dir / "run.systemd_unit").write_text(unit_name)
            (app_dir / "run.pid").write_text(str(pid))
            runtime_state.mark_vision_started(app_name, mode, config_name)
        except Exception:
            _processes.pop(app_name, None)
            _stop_systemd_unit(unit_name)
            (app_dir / "run.pid").unlink(missing_ok=True)
            control_sock.unlink(missing_ok=True)
            (app_dir / "run.boot_id").unlink(missing_ok=True)
            (app_dir / "run.systemd_unit").unlink(missing_ok=True)
            raise

        return pid


# ── 停止 ─────────────────────────────────────────────────────────────────────

def _stop_app_unlocked(app_name: str) -> bool:
    mp = _processes.get(app_name)
    disk_pid = _read_pid(app_name)
    if disk_pid is not None and (mp is None or mp.pid != disk_pid):
        mp = _recover_process(app_name)
    if not mp:
        # 程序包被外部覆盖后，run.pid 和内存状态可能已经丢失，但固定名称的
        # transient unit 仍在运行。Web 停止/覆盖流程必须能清理这种孤儿进程。
        unit_name = _app_unit_name(app_name)
        stopped = (
            _systemd_main_pid(unit_name) is not None
            and _stop_systemd_unit(unit_name)
        )
        _clear_stale_runtime_marker(app_name)
        return stopped

    stopped = False
    try:
        if mp.unit_name is not None:
            stopped = _stop_systemd_unit(mp.unit_name)
        if not stopped and mp.proc is not None:
            mp.proc.send_signal(signal.SIGTERM)
            try:
                mp.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                mp.proc.kill()
                mp.proc.wait()
            stopped = True
        elif not stopped:
            _terminate_pid(mp.pid)
            stopped = True
    except ProcessLookupError:
        stopped = True
    finally:
        _processes.pop(app_name, None)
        _clear_runtime_files_if_pid(app_name, mp.pid)

    return stopped


def stop_all_apps_for_install_unlocked() -> list[str]:
    """停止所有 Web 托管视觉程序；调用方必须已持有 ``runtime_lock``。

    上传程序包会在停止和目录替换期间持有同一把全局锁，保证另一请求不能在
    “检查完运行状态”后立即启动程序。除正常的 run.pid 状态外，这里也扫描固定
    前缀的 transient unit，用于清理目录曾被覆盖后遗留的孤儿视觉进程。
    """
    stopped_labels: set[str] = set()
    app_names: set[str] = set(_processes)
    unit_to_app: Dict[str, str] = {}

    if APPS_ROOT.exists():
        try:
            for entry in APPS_ROOT.iterdir():
                if not entry.is_dir() or entry.name.startswith((".", "_")):
                    continue
                app_names.add(entry.name)
                unit_to_app[_app_unit_name(entry.name)] = entry.name
        except OSError:
            pass

    for app_name in sorted(app_names):
        if _stop_app_unlocked(app_name):
            stopped_labels.add(app_name)

    # 兜底清理已经找不到 App 目录、因而无法从名称扫描到的 transient unit。
    for unit_name in _list_running_app_units():
        if not _stop_systemd_unit(unit_name):
            raise RuntimeError(f"无法停止正在运行的视觉进程服务: {unit_name}")
        stopped_labels.add(unit_to_app.get(unit_name, unit_name))

    remaining = _list_running_app_units()
    if remaining:
        raise RuntimeError(f"仍有视觉进程未停止: {', '.join(remaining)}")

    # 上传意味着用户明确要求当前视觉任务停止，避免控制台重启后又按旧意图恢复。
    for app_name in sorted(app_names):
        runtime_state.mark_vision_stopped(app_name)
        _clear_stale_runtime_marker(app_name)

    return sorted(stopped_labels)


def stop_app(app_name: str) -> bool:
    """停止视觉程序并持久化用户的“停止”意图。"""
    with _exclusive_start_lock():
        stopped = _stop_app_unlocked(app_name)
        runtime_state.mark_vision_stopped(app_name)
        return stopped


# ── 状态查询 ─────────────────────────────────────────────────────────────────

def get_status(app_name: str) -> dict:
    stopped = {"status": "stopped", "mode": None, "pid": None,
               "uptime_seconds": None, "config": None}

    mp = _processes.get(app_name)
    disk_pid = _read_pid(app_name)
    # 运行状态以服务器共享的PID标记为准。当前worker没有启动该进程，或另一个worker
    # 已重新启动出新PID时，立即从磁盘恢复，避免不同浏览器命中不同worker后状态不一致。
    if disk_pid is not None and (mp is None or mp.pid != disk_pid):
        mp = _recover_process(app_name)
    if not mp:
        return stopped

    if mp.proc is not None:
        if mp.proc.poll() is not None:
            _processes.pop(app_name, None)
            _clear_runtime_files_if_pid(app_name, mp.pid)
            return stopped
    else:
        if not _pid_belongs_to_app(app_name, mp.pid):
            _processes.pop(app_name, None)
            _clear_runtime_files_if_pid(app_name, mp.pid)
            return stopped

    return {
        "status":         "running",
        "mode":           mp.mode,
        "pid":            mp.pid,
        "uptime_seconds": int(time.time() - mp.started_at),
        "config":         mp.config,
    }
