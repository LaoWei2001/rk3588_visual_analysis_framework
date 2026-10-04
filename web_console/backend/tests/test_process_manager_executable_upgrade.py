import json
import os
import shutil
import subprocess

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
        replacement = app_dir / ".vision_analysis.next"
        shutil.copy2("/bin/true", replacement)
        os.replace(replacement, binary)

        assert os.readlink(f"/proc/{process.pid}/exe").endswith(" (deleted)")
        assert pm._pid_belongs_to_app("demo", process.pid) is True
    finally:
        process.terminate()
        process.wait(timeout=2)
