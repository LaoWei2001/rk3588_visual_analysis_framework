import subprocess

from services import process_manager as pm


class _Launcher:
    def __init__(self, returncode=0):
        self.returncode = returncode

    def poll(self):
        return self.returncode


def test_wait_accepts_successful_systemd_run_exit(monkeypatch):
    values = iter((None, 4321))
    monkeypatch.setattr(pm, "_systemd_main_pid", lambda _: next(values))
    monkeypatch.setattr(pm.time, "sleep", lambda _: None)
    assert pm._wait_for_systemd_main_pid("rkvision-test.service", _Launcher()) == 4321


def test_managed_start_routes_output_to_journal(monkeypatch, tmp_path):
    captured = {}

    def fake_popen(command, **kwargs):
        captured["command"] = command
        captured["kwargs"] = kwargs
        return _Launcher()

    monkeypatch.setattr(pm.subprocess, "Popen", fake_popen)
    pm._start_systemd_app(
        tmp_path / "vision_analysis",
        tmp_path / "assets/config.json",
        tmp_path,
        {"ASSETS_DIR": str(tmp_path / "assets")},
        "rkvision-test.service",
    )
    assert "--pipe" not in captured["command"]
    assert "--property=StandardOutput=journal" in captured["command"]
    assert "--property=StandardError=journal" in captured["command"]


def test_log_tail_reads_shared_journal(monkeypatch):
    result = subprocess.CompletedProcess([], 0, stdout="first\nsecond\n", stderr="")
    monkeypatch.setattr(pm.subprocess, "run", lambda *args, **kwargs: result)
    assert pm.get_logs("demo", 20) == ["first", "second"]
