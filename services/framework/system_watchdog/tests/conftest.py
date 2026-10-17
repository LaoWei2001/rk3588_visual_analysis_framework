from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'runtime_manager/python'))


@pytest.fixture(autouse=True)
def isolated_recovery_history(tmp_path, monkeypatch):
    from rk3588_runtime import recovery_history, reboot_coordinator
    monkeypatch.setattr(recovery_history, 'HISTORY_FILE', tmp_path / 'history/events.json')
    monkeypatch.setattr(reboot_coordinator, 'STATE_FILE', tmp_path / 'reboot/state.json')
    monkeypatch.setattr(reboot_coordinator, 'LEGACY_FILES', ())
    monkeypatch.setattr(reboot_coordinator, '_system_state', lambda: 'running')
    def forbidden_submit(*args, **kwargs):
        pytest.fail('Real reboot commands are forbidden in unit tests')
    monkeypatch.setattr(reboot_coordinator, '_submit', forbidden_submit)
