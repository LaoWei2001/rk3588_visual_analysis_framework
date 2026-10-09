"""Actual local files, bounded background counting, and receiver-owned snapshots."""
import importlib.util
import json
import threading
import time
from pathlib import Path

import pytest
from fastapi import HTTPException
from pydantic import ValidationError

from routers.dataset import Heartbeat
from services.dataset_manager import DatasetManager

SOURCE = Path(__file__).resolve().parents[1] / 'resources/dataset_receiver.py'
spec = importlib.util.spec_from_file_location('inventory_receiver', SOURCE)
pc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pc)


def photo(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b'\xff\xd8\xff\xd9')
    return path


def wait_snapshot(scanner):
    deadline = time.monotonic() + 2
    while scanner.snapshot() is None and time.monotonic() < deadline:
        time.sleep(.01)
    assert scanner.snapshot() is not None
    return scanner.snapshot()


def test_scan_counts_actual_channel_files_and_manual_deletion(tmp_path):
    scanner = pc.InventoryScanner(tmp_path, 'device')
    assert scanner.scan() == []
    first = photo(tmp_path/'device/demo/ch0/0_29/first.jpg')
    second = photo(first.with_name('second.JPG'))
    photo(tmp_path/'device/demo/ch0/30_59/nested/third.jpeg')
    photo(tmp_path/'device/demo/ch3/0_29/other.jpg')
    photo(tmp_path/'device/other/ch0/0_29/other-program.jpg')
    photo(tmp_path/'another-device/demo/ch0/0_29/ignored.jpg')
    photo(first.with_name('unfinished.jpg.part'))
    photo(first.with_name('notes.txt'))
    photo(tmp_path/'device/demo/ch99/ignored.jpg')
    photo(tmp_path/'device/.hidden/ch0/ignored.jpg')
    outside = photo(tmp_path/'outside/external.jpg')
    first.with_name('linked.jpg').symlink_to(outside)
    (first.parent/'linked-folder').symlink_to(outside.parent, target_is_directory=True)
    def counts():
        return {(row['app'], row['channel_id']): row['count'] for row in scanner.scan()}
    assert counts() == {('demo', 0): 3, ('demo', 3): 1, ('other', 0): 1}
    first.rename(first.with_name('renamed.jpg'))
    assert counts()[('demo', 0)] == 3
    second.unlink()
    assert counts()[('demo', 0)] == 2
    pc.shutil.rmtree(tmp_path/'device/demo/ch0')
    assert ('demo', 0) not in counts() # A completed snapshot maps absent channels to zero.


def test_background_scan_does_not_block_receive_or_exit(tmp_path, monkeypatch):
    receiver = pc.Receiver('http://device', tmp_path)
    entered, release = threading.Event(), threading.Event()
    def blocked():
        entered.set()
        release.wait(5)
        return []
    monkeypatch.setattr(receiver.inventory, 'scan', blocked)
    calls = []
    def request(path, body=None, binary=False):
        calls.append((path, body))
        return {'samples': []}
    monkeypatch.setattr(receiver, 'request', request)
    receiver.inventory.start()
    try:
        assert entered.wait(1)
        started = time.monotonic()
        assert receiver.cycle() == 0
        receiver.inventory.stop()
        assert time.monotonic() - started < 1
        assert calls[0][1]['inventory'] is None
    finally:
        release.set()
        receiver.inventory._worker.join(timeout=2)
        receiver.db.close()


def test_unreadable_directory_reports_unknown_instead_of_zero(tmp_path, monkeypatch):
    scanner = pc.InventoryScanner(tmp_path, 'device')
    def denied():
        raise PermissionError('directory unavailable')
    monkeypatch.setattr(scanner, 'scan', denied)
    scanner.start()
    try:
        snapshot = wait_snapshot(scanner)
        assert snapshot['counts'] is None
        assert 'directory unavailable' in snapshot['error']
    finally:
        scanner.stop()
        scanner._worker.join(timeout=2)


@pytest.fixture
def manager(tmp_path, monkeypatch):
    manager = DatasetManager(tmp_path/'board')
    with manager.db() as db:
        for app, task, channel in [('demo', 'task-zero', 0), ('demo', 'task-three', 3), ('other', 'other-task', 0)]:
            db.execute('INSERT INTO tasks VALUES (?,?,?,?,?,?,?,?)', (task, app, channel, json.dumps({}), 'revision', 0, 100, 0))
    monkeypatch.setattr(manager, 'sync', lambda _: ({'tasks': []}, ''))
    monkeypatch.setattr(manager, 'channels', lambda _: [])
    return manager


def report(scan_id, count):
    return {'scan_id': scan_id, 'counts': [{'app': 'demo', 'channel_id': 0, 'count': count}], 'error': ''}


def test_current_counts_decrease_without_reducing_cumulative_progress(manager):
    assert manager.list('demo')['tasks'][0]['current_files'] is None
    manager.heartbeat('first', 'D:/samples', '', report('one', 10))
    tasks = manager.list('demo')['tasks']
    assert [t['current_files'] for t in tasks] == [10, 0]
    assert manager.list('other')['tasks'][0]['current_files'] == 0
    assert all(t['saved'] == 100 for t in tasks)
    first_time = tasks[0]['files_updated_at']
    manager.heartbeat('first', 'D:/samples', '', report('one', 10))
    assert manager.list('demo')['tasks'][0]['files_updated_at'] == first_time
    manager.heartbeat('first', 'D:/samples', '', report('two', 0))
    assert manager.list('demo')['tasks'][0]['current_files'] == 0
    assert all(row['saved'] == 100 for row in manager.rows())
    manager.disconnect('first')
    offline = manager.list('demo')
    assert not offline['receiver']['online'] and offline['tasks'][0]['current_files'] == 0
    assert DatasetManager(manager.root).settings('receiver')['inventory'] == report('two', 0)


def test_replacement_directory_and_legacy_receiver_do_not_reuse_old_counts(manager):
    manager.heartbeat('first', 'D:/samples', '', report('one', 10))
    manager.disconnect('first')
    manager.heartbeat('second', 'D:/new', '')
    assert manager.list('demo')['tasks'][0]['current_files'] is None
    with pytest.raises(HTTPException) as error:
        manager.heartbeat('first', 'D:/samples', '', report('old', 999))
    assert error.value.status_code == 409
    assert manager.list('demo')['tasks'][0]['current_files'] is None
    manager.heartbeat('second', 'D:/new', '', report('new', 2))
    assert manager.list('demo')['tasks'][0]['current_files'] == 2
    manager.heartbeat('second', 'D:/new', '') # An older EXE does not report an inventory.
    assert manager.list('demo')['tasks'][0]['current_files'] is None
    manager.heartbeat('second', 'D:/new', '', {'scan_id': 'failed', 'counts': None, 'error': 'disk unavailable'})
    task = manager.list('demo')['tasks'][0]
    assert task['current_files'] is None and task['files_error'] == 'disk unavailable'


@pytest.mark.parametrize('item', [
    {'app': '../outside', 'channel_id': 0, 'count': 1},
    {'app': 'demo', 'channel_id': 32, 'count': 1},
    {'app': 'demo', 'channel_id': False, 'count': 1},
    {'app': 'demo', 'channel_id': 0, 'count': -1},
    {'app': 'demo', 'channel_id': 0, 'count': 1.5},
    {'app': 'demo', 'channel_id': 0, 'count': True},
])
def test_inventory_request_validation(item):
    with pytest.raises(ValidationError):
        Heartbeat(client='receiver', inventory={'scan_id': 'scan', 'counts': [item]})


def test_old_receiver_heartbeat_stays_compatible():
    assert Heartbeat(client='legacy').inventory is None
