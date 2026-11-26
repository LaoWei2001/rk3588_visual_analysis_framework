"""Exercise port selection and rollback without modifying the board's service."""
import importlib.util
from pathlib import Path
import socket
import subprocess

import pytest


SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"
spec = importlib.util.spec_from_file_location("console_port", SCRIPTS / "console_port.py")
ports = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ports)


@pytest.mark.parametrize("value", ["0", "65536", "-1", "abc", "", "1.5"])
def test_invalid_port(value):
    result = subprocess.run(["python3", spec.origin, "select", "--port", value], capture_output=True)
    assert result.returncode == 2


def test_selection_checks_occupied_ports(monkeypatch, tmp_path):
    monkeypatch.setattr(ports, "DROP_IN", tmp_path / "port.conf")
    monkeypatch.setattr(ports, "running_port", lambda: None)
    monkeypatch.setattr(ports, "unit_property", lambda name: "")
    assert ports.configured_port() == 8080
    with socket.socket() as busy:
        busy.bind(("127.0.0.1", 0))
        busy.listen()
        port = busy.getsockname()[1]
        with pytest.raises(RuntimeError, match=str(port)):
            ports.check_available(port)
        monkeypatch.setattr(ports, "running_port", lambda: port)
        ports.check_available(port)


@pytest.mark.parametrize("legacy", [True, False])
def test_persistence_and_failed_restart(tmp_path, monkeypatch, legacy):
    fragment = tmp_path / ports.UNIT
    dropin = tmp_path / (ports.UNIT + ".d") / "99-web-console-port.conf"
    initial = "8080" if legacy else "${RK_CONSOLE_PORT}"
    command = f"/usr/bin/python3 -m uvicorn main:app --port {initial} --workers 1"
    fragment.write_text(f"[Service]\nExecStart={command}\n")
    monkeypatch.setattr(ports, "DROP_IN", dropin)
    state = {"port": 8080, "restarts": 0}

    def prop(name):
        override = dropin.read_text() if dropin.exists() else ""
        if name == "FragmentPath":
            return str(fragment)
        if name == "Environment":
            for line in override.splitlines():
                if line.startswith("Environment="):
                    return line.split("=", 1)[1]
            return "" if legacy else "RK_CONSOLE_PORT=8080"
        if name == "ExecStart":
            return next((line for line in override.splitlines() if line.startswith("ExecStart=/")), command)
        raise AssertionError(name)

    def systemctl(args, *, check):
        if args[1] == "restart":
            state["restarts"] += 1
            state["port"] = ports.configured_port()
            if state["port"] == 9091:
                raise subprocess.CalledProcessError(1, args)
        else:
            assert args[1] == "daemon-reload"

    def ready(port):
        assert port == state["port"]

    monkeypatch.setattr(ports, "running_port", lambda: state["port"])
    monkeypatch.setattr(ports, "unit_property", prop)
    monkeypatch.setattr(ports, "check_available", lambda port: None)
    monkeypatch.setattr(ports.subprocess, "run", systemctl)
    monkeypatch.setattr(ports, "wait_ready", ready)
    ports.apply_port(9090, force_restart=False)
    assert ports.configured_port() == state["port"] == 9090
    assert ("ExecStart=" in dropin.read_text()) == legacy
    previous = dropin.read_bytes()
    ports.apply_port(9090, force_restart=False)
    assert state["restarts"] == 1
    ports.apply_port(ports.configured_port(), force_restart=True)
    assert state["restarts"] == 2
    with pytest.raises(subprocess.CalledProcessError):
        ports.apply_port(9091, force_restart=False)
    assert previous == dropin.read_bytes()
    assert state["port"] == 9090
