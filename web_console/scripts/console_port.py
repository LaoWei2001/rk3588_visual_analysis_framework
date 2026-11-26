"""Select and persist the Web console listener for source and offline installations."""

import argparse
import fcntl
import http.client
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time

UNIT = "rk3588-console.service"
DROP_IN = Path("/etc/systemd/system") / (UNIT + ".d") / "99-web-console-port.conf"
DEFAULT_PORT = 8080


def port_number(text):
    if not re.fullmatch(r"[0-9]{1,5}", text) or not 1 <= int(text) <= 65535:
        raise argparse.ArgumentTypeError("端口必须是 1 至 65535 的整数")
    return int(text)


def unit_property(name):
    result = subprocess.run(["systemctl", "show", UNIT, "-p", name, "--value"],
                            capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else ""


def configured_port():
    # Also recognize a manually configured numeric --port on an older board.
    command = unit_property("ExecStart")
    match = re.search(r"--port[ =](\d+)\b", command)
    if not match:
        match = re.search(r"(?:^|\s)RK_CONSOLE_PORT=(\d+)\b", unit_property("Environment"))
    if not match and DROP_IN.is_file():
        match = re.search(r"^Environment=RK_CONSOLE_PORT=(\d+)$", DROP_IN.read_text(), re.M)
    return int(match[1]) if match else DEFAULT_PORT


def running_port():
    pid = unit_property("MainPID")
    if pid.isdecimal() and int(pid) > 0:
        try:
            args = (Path("/proc") / pid / "cmdline").read_bytes().split(b"\0")
            for i, arg in enumerate(args):
                if arg == b"--port":
                    return int(args[i + 1])
                if arg.startswith(b"--port="):
                    return int(arg.split(b"=", 1)[1])
        except (OSError, ValueError, IndexError):
            pass
    return None


def check_available(port):
    if running_port() == port:
        return
    with socket.socket() as probe:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            probe.bind(("0.0.0.0", port))
        except OSError as error:
            raise RuntimeError(f"Web 端口 {port} 无法监听：{error}") from error


def port_override(port):
    content = f"[Service]\nEnvironment=RK_CONSOLE_PORT={port}\n"
    # A front-end-only update may encounter a pre-upgrade unit with a literal
    # port. Keep its executable and flags; replace just the port argument.
    unit = Path(unit_property("FragmentPath")).read_text()
    command = next((line for line in unit.splitlines() if line.startswith("ExecStart=")), "")
    if "${RK_CONSOLE_PORT}" not in command:
        command, count = re.subn(r"(--port[ =])\d+\b", r"\g<1>${RK_CONSOLE_PORT}", command)
        if count != 1:
            raise RuntimeError(f"无法识别 {UNIT} 中的端口参数，请检查其 ExecStart")
        content += f"ExecStart=\n{command}\n"
    return content.encode()


def write_override(content):
    with tempfile.NamedTemporaryFile(dir=DROP_IN.parent, delete=False) as tmp:
        path = Path(tmp.name)
        try:
            tmp.write(content)
            tmp.flush()
            os.fchmod(tmp.fileno(), 0o644)
            path.replace(DROP_IN)
        finally:
            path.unlink(missing_ok=True)


def wait_ready(port):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=1)
        try:
            conn.request("GET", "/health")
            response = conn.getresponse()
            if response.status == 200 and json.loads(response.read()).get("status") == "ok" and running_port() == port:
                return
        except (OSError, http.client.HTTPException, ValueError, AttributeError):
            pass
        finally:
            conn.close()
        time.sleep(0.25)
    raise RuntimeError(f"控制台未在端口 {port} 就绪，请检查 journalctl -u {UNIT}")


def apply_port(port, force_restart):
    DROP_IN.parent.mkdir(parents=True, exist_ok=True)
    with (DROP_IN.parent / ".port.lock").open("a+b") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        check_available(port)
        current = configured_port()
        active = running_port()
        previous = DROP_IN.read_bytes() if DROP_IN.is_file() else None
        desired = port_override(port) if port != current or previous is not None else None
        changed = desired != previous
        if not changed and not force_restart and current == active == port:
            return
        if changed:
            write_override(desired)
        try:
            subprocess.run(["systemctl", "daemon-reload"], check=True)
            subprocess.run(["systemctl", "restart", UNIT], check=True)
            wait_ready(port)
        except (OSError, RuntimeError, subprocess.CalledProcessError):
            if changed:
                if previous is None:
                    DROP_IN.unlink()
                else:
                    write_override(previous)
                subprocess.run(["systemctl", "daemon-reload"], check=True)
                if active is not None:
                    subprocess.run(["systemctl", "restart", UNIT], check=True)
                    wait_ready(active)
                print("已恢复原端口配置。", file=sys.stderr)
            raise


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False, add_help=False)
    parser.add_argument("action", choices=["show", "select", "stage", "install", "update"])
    parser.add_argument("--port", type=port_number)
    args = parser.parse_args()
    port = configured_port() if args.port is None else args.port
    if args.action == "select":
        check_available(port)
    elif args.action == "stage":
        # The offline deb can also be unpacked in an image without a running systemd.
        DROP_IN.parent.mkdir(parents=True, exist_ok=True)
        write_override(f"[Service]\nEnvironment=RK_CONSOLE_PORT={port}\n".encode())
    elif args.action in ("install", "update"):
        apply_port(port, force_restart=args.action == "install")
    print(port)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        sys.exit(f"[错误] {exc}")
