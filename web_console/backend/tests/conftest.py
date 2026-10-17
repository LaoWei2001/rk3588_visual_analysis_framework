import pytest


@pytest.fixture(autouse=True)
def isolated_recovery_history(tmp_path, monkeypatch):
    from services.runtime_loader import load_runtime
    history = load_runtime('recovery_history')
    monkeypatch.setattr(history, 'HISTORY_FILE', tmp_path / 'history/events.json')
    diagnostics = load_runtime('fault_diagnostics')
    monkeypatch.setattr(diagnostics, 'STORE', tmp_path / 'history/diagnostics.json')
    monkeypatch.setattr(load_runtime('performance'), 'PROFILE', tmp_path / 'performance/config.json')
