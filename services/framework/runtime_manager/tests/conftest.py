from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'python'))


@pytest.fixture(autouse=True)
def isolated_recovery_history(tmp_path, monkeypatch):
    from rk3588_runtime import recovery_history, fault_diagnostics, performance, reboot_coordinator
    monkeypatch.setattr(recovery_history, 'HISTORY_FILE', tmp_path / 'history/events.json')
    monkeypatch.setattr(fault_diagnostics, 'STORE', tmp_path / 'history/diagnostics.json')
    monkeypatch.setattr(performance, 'PROFILE', tmp_path / 'performance/config.json')
    monkeypatch.setattr(reboot_coordinator, 'STATE_FILE', tmp_path / 'reboot/state.json')
    monkeypatch.setattr(reboot_coordinator, 'LEGACY_FILES', (tmp_path / 'legacy-network.json', tmp_path / 'legacy-kernel.json'))
    monkeypatch.setattr(reboot_coordinator, '_system_state', lambda: 'running')
    def forbidden_submit(*args, **kwargs):
        pytest.fail('Real reboot commands are forbidden in unit tests')
    monkeypatch.setattr(reboot_coordinator, '_submit', forbidden_submit)
