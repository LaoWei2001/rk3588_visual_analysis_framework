import json
import os
import shutil
import signal
import subprocess
from contextlib import contextmanager

import pytest
from fastapi import HTTPException
from routers import stream
from services import app_scanner

from services import process_manager as pm


def test_display_setting_is_read_without_modifying_config(tmp_path):
    config = tmp_path / "config.json"
    content = {"global": {"enable_display": 1}, "channels": []}
    config.write_text(json.dumps(content, ensure_ascii=False, indent=2), encoding="utf-8")
    before = config.read_bytes()

    assert pm._config_enables_display(config) is True
    assert config.read_bytes() == before


def test_display_setting_defaults_to_disabled(tmp_path):
    missing_global = tmp_path / "missing-global.json"
    missing_global.write_text("{}", encoding="utf-8")
    disabled = tmp_path / "disabled.json"
    disabled.write_text('{"global":{"enable_display":0}}', encoding="utf-8")

    assert pm._config_enables_display(missing_global) is False
    assert pm._config_enables_display(disabled) is False


def test_running_process_survives_atomic_executable_upgrade(tmp_path, monkeypatch):
    app_dir = tmp_path / "demo"
    app_dir.mkdir()
    binary = app_dir / "vision_analysis"
    shutil.copy2("/bin/sleep", binary)
    monkeypatch.setattr(pm, "APPS_ROOT", tmp_path)
    monkeypatch.setattr(pm, "BINARY_NAME", "vision_analysis")

    process = subprocess.Popen([str(binary), "10"], cwd=app_dir)
    try:
        monkeypatch.setattr(pm, '_processes', {'demo': pm.ManagedProcess(
            app_name='demo', pid=process.pid, started_at=0, proc=process)})
        identity = pm.get_process_identity('demo')
        assert identity is not None and str(process.pid) in identity
        replacement = app_dir / ".vision_analysis.next"
        shutil.copy2("/bin/true", replacement)
        os.replace(replacement, binary)

        assert os.readlink(f"/proc/{process.pid}/exe").endswith(" (deleted)")
        assert pm._pid_belongs_to_app("demo", process.pid) is True
        assert pm.get_process_identity('demo') == identity
    finally:
        process.terminate()
        process.wait(timeout=2)
    assert pm.get_process_identity('demo') is None


@pytest.fixture
def cli_app(tmp_path, monkeypatch):
    app = tmp_path / "demo"
    (app / "assets").mkdir(parents=True)
    # cat accepts a real config filename and stays alive on stdin afterwards.
    shutil.copy2("/bin/cat", app / "vision_analysis")
    (app / "run.config").write_text("previous.json")
    (app / "assets/config.json").write_text('{"global":{"enable_rtsp":false}}')
    (app / "assets/live.json").write_text('{"global":{"enable_rtsp":true,"rtsp_port":9554,"rtsp_path":"/camera"}}')
    for module in (pm, app_scanner, stream):
        monkeypatch.setattr(module, "APPS_ROOT", tmp_path)
    monkeypatch.setattr(app_scanner, "get_vision_settings", lambda _: dict(autostart=False, desired_running=False))
    monkeypatch.setattr(app_scanner, "data_dir", lambda name: tmp_path / ".data" / name)
    return app


@contextmanager
def terminal_process(app, *args, cwd=None):
    with subprocess.Popen([str(app / "vision_analysis"), *args], cwd=cwd or app,
                          stdin=subprocess.PIPE, stdout=subprocess.DEVNULL) as process:
        try:
            yield process
        finally:
            process.send_signal(signal.SIGINT)
            process.wait(timeout=3)


def test_cli_lifecycle_and_preview_use_current_instance(cli_app):
    with terminal_process(cli_app, "./assets/live.json", "-") as process:
        status, = app_scanner.scan_apps()
        assert status["status"] == "running" and status["pid"] == process.pid
        assert status["launch_source"] == "command_line" and status["config"] == "live.json"
        assert status["uptime_seconds"] >= 0
        assert stream._rtsp_url("demo") == "rtsp://127.0.0.1:9554/camera"
        with pytest.raises(pm.CommandLineAppRunningError):
            pm.start_app("demo", "config.json")
        with pytest.raises(pm.CommandLineAppRunningError):
            pm.stop_app_detailed("demo")
        assert process.poll() is None
        assert not (cli_app / "run.pid").exists()
        assert (cli_app / "run.config").read_text() == "previous.json"
    assert pm.get_status("demo")["status"] == "stopped"
    with terminal_process(cli_app):
        assert pm.get_status("demo")["config"] == "config.json"
        with pytest.raises(HTTPException, match="未启用实时预览"):
            stream._rtsp_url("demo")
    with terminal_process(cli_app, str(cli_app / "assets/live.json"), "-", cwd=cli_app.parent):
        assert pm.get_status("demo")["config"] == "live.json"


def test_cli_tools_and_external_config_never_select_stale_preview(cli_app):
    with terminal_process(cli_app, "-u"):
        assert pm.get_status("demo")["status"] == "stopped"
    external = cli_app.parent / "live.json"
    external.write_text("{}")
    with terminal_process(cli_app, str(external), "-"):
        assert pm.get_status("demo")["status"] == "running"
        assert pm.get_status("demo")["config"] is None
        with pytest.raises(HTTPException) as exc:
            stream._rtsp_url("demo")
        assert exc.value.status_code == 409
