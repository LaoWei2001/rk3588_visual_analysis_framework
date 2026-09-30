import os
import shutil
import subprocess

from services import process_manager as pm


def test_running_process_survives_atomic_executable_upgrade(tmp_path, monkeypatch):
    app_dir = tmp_path / "demo"
    app_dir.mkdir()
    binary = app_dir / "vision_analysis"
    shutil.copy2("/bin/sleep", binary)
    monkeypatch.setattr(pm, "APPS_ROOT", tmp_path)
    monkeypatch.setattr(pm, "BINARY_NAME", "vision_analysis")

    process = subprocess.Popen([str(binary), "10"], cwd=app_dir)
    try:
        replacement = app_dir / ".vision_analysis.next"
        shutil.copy2("/bin/true", replacement)
        os.replace(replacement, binary)

        assert os.readlink(f"/proc/{process.pid}/exe").endswith(" (deleted)")
        assert pm._pid_belongs_to_app("demo", process.pid) is True
    finally:
        process.terminate()
        process.wait(timeout=2)

