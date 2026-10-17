import json
import os

from services import network_manager
from services.runtime_loader import load_runtime


def test_network_transaction_retention_preserves_active_and_latest_diagnostics(tmp_path, monkeypatch):
    monkeypatch.setattr(network_manager, 'TRANSACTION_DIR', tmp_path)
    for index in range(25):
        path = tmp_path / (f'{index:032x}' + '.json')
        path.write_text(json.dumps({'id': f'{index:032x}', 'status': 'confirmed'}))
        os.utime(path, (100 + index, 100 + index))
    active = tmp_path / ('a' * 32 + '.json')
    active.write_text(json.dumps({'id': 'a' * 32, 'status': 'awaiting_confirmation'}))
    os.utime(active, (0, 0))
    config = tmp_path / 'unrelated.json'
    config.write_text('{"keep": true}')
    network_manager._prune_completed_transactions()
    assert active.exists() and config.exists()
    completed = [path for path in tmp_path.glob('*.json') if path not in (active, config)]
    assert len(completed) == 20
    assert not (tmp_path / ('0' * 32 + '.json')).exists()


def test_current_completed_transaction_survives_backwards_clock_jump(tmp_path, monkeypatch):
    monkeypatch.setattr(network_manager, 'TRANSACTION_DIR', tmp_path)
    for index in range(30):
        path = tmp_path / (f'{index:032x}' + '.json')
        path.write_text(json.dumps({'id': f'{index:032x}', 'status': 'confirmed'}))
    current = tmp_path / ('f' * 32 + '.json')
    current.write_text(json.dumps({'id': 'f' * 32, 'status': 'rolled_back'}))
    os.utime(current, (0, 0))
    network_manager._prune_completed_transactions(current)
    assert current.exists() and len(list(tmp_path.glob('*.json'))) == 20


def test_network_rollback_is_a_structured_recovery_record(tmp_path, monkeypatch):
    monkeypatch.setattr(network_manager, 'TRANSACTION_DIR', tmp_path)
    network_manager._write_transaction({'id': 'a' * 32, 'status': 'rolled_back', 'device': 'eth0',
                                        'failure_reason': '用户未确认网络切换'})
    rows = load_runtime('recovery_history').snapshot('network')['events']
    assert len(rows) == 1 and rows[0]['reason'] == '用户未确认网络切换'
    assert '恢复原连接' in rows[0]['action'] and rows[0]['result'] == 'performed'


def test_user_restart_saves_success_and_failure_without_restarting_real_app(monkeypatch):
    import asyncio
    import pytest
    from fastapi import HTTPException
    from routers import process
    history = load_runtime('recovery_history')
    monkeypatch.setattr(process, '_sync_background_services', lambda: {})
    monkeypatch.setattr(process.pm, 'start_app', lambda *args: 123)
    asyncio.run(process.restart_app('demo', process.StartRequest()))
    row = history.snapshot('application')['events'][0]
    assert row['result'] == 'performed' and '用户' in row['reason'] and '123' in row['detail']
    def fail(*args):
        raise RuntimeError('test startup failure')
    monkeypatch.setattr(process.pm, 'start_app', fail)
    with pytest.raises(HTTPException):
        asyncio.run(process.restart_app('demo', process.StartRequest()))
    row = history.snapshot('application')['events'][0]
    assert row['result'] == 'failed' and row['detail'] == 'test startup failure'
