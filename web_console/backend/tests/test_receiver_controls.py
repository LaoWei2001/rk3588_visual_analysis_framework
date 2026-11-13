"""Default PC controls, real low-latency control transport and Win32 key lifecycle."""
import ctypes
import hashlib
import json
import socket
import threading
import time
from urllib.error import HTTPError
from urllib.request import Request, build_opener, ProxyHandler

urlopen = build_opener(ProxyHandler({})).open

import pytest
import uvicorn
from fastapi import HTTPException

from test_remote_dataset import environment, config, pc  # noqa: F401


@pytest.fixture
def connected(environment):
    manager, _, computer = environment
    task = manager.save('demo', 0, config(limit=20))['tasks'][0]
    receiver = pc.Receiver('http://device', computer)
    receiver.hotkeys.available = True  # No Windows desktop on the RK3588 test host.
    manager.heartbeat(receiver.client, str(computer), '', control_hash=receiver.poll_body()['control_hash'])
    yield manager, task, receiver
    receiver.db.close()


@pytest.mark.parametrize('enabled', [False, True])
def test_single_live_task_is_selected_without_web_and_keeps_auto_state(connected, enabled):
    manager, task, receiver = connected
    if enabled: manager.set_enabled('demo', task['id'], True)
    state = manager.receiver_control(receiver.client, receiver.control_key, 'status', '', 'r')
    assert state['binding']['task_id'] == task['id'] and state['binding']['channel_id'] == 0
    assert state['enabled'] == enabled
    assert bool(manager.rows()[0]['enabled']) == enabled
    assert receiver.control_key not in json.dumps(state) and 'key_hash' not in json.dumps(state)
    assert manager.receiver_control(receiver.client, receiver.control_key, 'status', '', 'r')['binding'] == state['binding']


def test_multiple_tasks_require_pc_selection_and_refresh_edits(connected):
    manager, task, receiver = connected
    second = next(t for t in manager.save('demo', 3, config())['tasks'] if t['channel_id'] == 3)
    def control(action='status', binding='', target=''):
        return manager.receiver_control(receiver.client, receiver.control_key, action, binding, 'r', target)
    state = control()
    assert state['binding'] is None and len(state['targets']) == 2
    with pytest.raises(HTTPException): control('capture')
    selected = control('select', target=second['id'])['binding']
    assert selected['channel_id'] == 3 and control()['binding'] == selected
    assert not any(row['enabled'] for row in manager.rows())
    manager.save('demo', 3, dict(config(), interval_sec=3))
    refreshed = control()['binding']
    assert refreshed['task_id'] == second['id'] and refreshed['id'] != selected['id']
    with pytest.raises(HTTPException): control('capture', selected['id'])
    manager.remove('demo', second['id'])
    assert control()['binding']['task_id'] == task['id']
    with pytest.raises(HTTPException): control('select', target=second['id'])


def test_stopped_and_deleted_programs_are_not_shortcut_candidates(connected, monkeypatch):
    manager, task, receiver = connected
    with manager.db() as db:
        db.execute('INSERT INTO tasks VALUES (?,?,?,?,?,?,?,?)',
                   ('old-task', 'removed-program', 0, json.dumps(config()), 'v1', 0, 0, 0))
    state = manager.receiver_control(receiver.client, receiver.control_key, 'status', '', 'r')
    assert [t['task_id'] for t in state['targets']] == [task['id']]
    monkeypatch.setattr(manager, 'process_identity', lambda app: None)
    state = manager.receiver_control(receiver.client, receiver.control_key, 'status', '', 'r')
    assert state['targets'] == [] and state['binding'] is None


def test_public_heartbeat_cannot_replace_active_receivers_key(connected):
    manager, task, receiver = connected
    forged_key = 'f' * 64
    for advertised in (receiver.poll_body()['control_hash'], ''):
        manager.heartbeat(receiver.client, 'same receiver', '', control_hash=advertised)
        with pytest.raises(HTTPException) as error:
            manager.heartbeat(receiver.client, 'copied identity', '', control_hash=hashlib.sha256(forged_key.encode()).hexdigest())
        assert error.value.status_code == 403
    with pytest.raises(HTTPException):
        manager.receiver_control(receiver.client, forged_key, 'status', '', 'r')


def test_disconnect_and_legacy_receiver_disable_controls(connected):
    manager, task, receiver = connected
    manager.receiver_control(receiver.client, receiver.control_key, 'status', '', 'r')
    manager.disconnect(receiver.client)
    with pytest.raises(HTTPException):
        manager.receiver_control(receiver.client, receiver.control_key, 'status', '', 'r')
    manager.heartbeat(receiver.client, str(receiver.directory), '')
    assert not manager.receiver_status()['hotkeys_available']
    with pytest.raises(HTTPException):
        manager.receiver_control(receiver.client, receiver.control_key, 'status', '', 'r')


def test_real_http_default_controls_and_capture_while_paused(connected):
    manager, task, receiver = connected
    import main
    listener = socket.socket(); listener.bind(('127.0.0.1', 0))
    address = f'http://127.0.0.1:{listener.getsockname()[1]}'
    server = uvicorn.Server(uvicorn.Config(main.app, log_level='error', log_config=None, lifespan='off', timeout_keep_alive=1))
    thread = threading.Thread(target=lambda: server.run(sockets=[listener]), daemon=True); thread.start()
    for _ in range(100):
        if server.started: break
        time.sleep(.02)
    connection = pc.ControlConnection(address, receiver.client, receiver.control_key)
    try:
        # No console login or web binding step. Normal task configuration still requires login.
        with pytest.raises(HTTPError) as error:
            urlopen(address + '/api/apps/demo/dataset')
        assert error.value.code == 401
        with pytest.raises(HTTPError) as error:
            urlopen(Request(address + '/api/dataset-receiver/control',
                            data=json.dumps({'client':receiver.client, 'action':'status', 'request_id':'r'}).encode(),
                            headers={'Content-Type':'application/json'}))
        assert error.value.code == 403
        binding = connection.request('status')['binding']
        assert binding['task_id'] == task['id']
        assert not manager.rows()[0]['enabled']
        assert connection.request('toggle', binding['id'], 'same-toggle')['enabled']
        assert connection.request('toggle', binding['id'], 'same-toggle')['enabled']
        assert not connection.request('toggle', binding['id'], 'pause')['enabled']
        time.sleep(1.2)  # Server-expired keep-alive is discarded before sending.
        result = connection.request('capture', binding['id'], 'manual-once')
        assert result['task']['manual_id'] == 'manual-once'
        for _ in range(100):
            captured = connection.request('result', binding['id'])['task']
            if captured.get('manual_state') == 'captured': break
            time.sleep(.02)
        assert captured['manual_state'] == 'captured' and captured['manual_wait_ms'] < 750
        assert not manager.rows()[0]['enabled']
        receiver.url = address
        receiver.opener = build_opener(ProxyHandler({}))
        assert receiver.cycle() >= 1
        assert list(receiver.directory.rglob('*.jpg'))
        second = next(t for t in manager.save('demo', 3, config())['tasks'] if t['channel_id'] == 3)
        selected = connection.request('select', task_id=second['id'])['binding']
        assert selected['task_id'] == second['id']
        with pytest.raises(RuntimeError, match='目标已变化'):
            connection.request('capture', binding['id'])
    finally:
        connection.close()
        server.should_exit = True; thread.join(timeout=5); listener.close()


def test_receiver_secret_survives_restart_but_is_not_advertised(tmp_path):
    first = pc.Receiver('http://device', tmp_path)
    key, client = first.control_key, first.client
    first.hotkeys.available = True
    body = first.poll_body()
    assert key not in json.dumps(body) and body['control_hash'] == hashlib.sha256(key.encode()).hexdigest()
    first.db.close()
    second = pc.Receiver('http://device', tmp_path)
    assert (second.control_key, second.client) == (key, client)
    assert second.poll_body()['control_hash'] == ''
    second.db.close()


def test_key_dispatch_is_immediate_and_not_queued_behind_downloads(monkeypatch):
    keys = pc.GlobalCaptureKeys('http://device', 'pc', 'a' * 64)
    keys.binding = dict(id='binding', app='demo', channel_id=0)
    keys._last_seen = time.monotonic()
    entered, release = threading.Event(), threading.Event()
    calls = []
    class Connection:
        def __init__(self, *args): pass
        def request(self, *args):
            calls.append(args); entered.set(); release.wait(2)
            return {'task': {'manual_state':'waiting'}}
        def close(self): pass
    monkeypatch.setattr(pc, 'ControlConnection', Connection)
    worker = threading.Thread(target=keys._commands); worker.start()
    try:
        assert keys.trigger('capture')
        assert entered.wait(.2)  # No one-second receiver polling delay.
        assert not keys.trigger('capture')
        assert calls[0][0:2] == ('capture', 'binding')
    finally:
        keys.stop(); release.set(); worker.join(3)
    assert len(calls) == 1


@pytest.mark.parametrize('conflict', [0, 2, 3])
def test_win32_register_no_repeat_unregister_and_message_dispatch(monkeypatch, conflict):
    from ctypes import wintypes
    keys = pc.GlobalCaptureKeys('http://device', 'pc', 'a' * 64)
    calls, dispatched = [], []
    class Function:
        def __init__(self, fn): self.fn = fn
        def __call__(self, *args): return self.fn(*args)
    class DLL: pass
    user, kernel = DLL(), DLL()
    def register(hwnd, identifier, modifiers, key):
        calls.append(('register', identifier, modifiers, key))
        return identifier != conflict
    user.RegisterHotKey = Function(register)
    user.UnregisterHotKey = Function(lambda _, identifier: calls.append(('unregister', identifier)))
    events = iter([(0x0312,1,1000), (0x0312,2,1000), (0x0312,3,1000), (0x0312,1,0), (0,0,0)])
    def get_message(ptr, *args):
        message, param, stamp = next(events)
        ptr._obj.message, ptr._obj.wParam, ptr._obj.time = message, param, stamp
        return int(message != 0)
    user.GetMessageW = Function(get_message)
    kernel.GetCurrentThreadId = Function(lambda: 77)
    kernel.GetTickCount = Function(lambda: 1000)
    monkeypatch.setattr(ctypes, 'WinDLL', lambda name, **kw: user if name == 'user32' else kernel, raising=False)
    monkeypatch.setattr(ctypes, 'get_last_error', lambda: 1409, raising=False)
    monkeypatch.setattr(keys, 'trigger', dispatched.append)
    keys._listen()
    assert calls[:2] == [('register',1,0x4000,0x77), ('register',2,0x4000,0x78)]
    assert ('unregister',1) in calls
    assert (('unregister',2) in calls) == (conflict != 2)
    assert (('unregister',3) in calls) == (not conflict)
    assert dispatched == ([] if conflict else ['capture','toggle','select'])
    assert not keys.available


def test_f7_selects_next_target_without_requiring_existing_binding(monkeypatch):
    keys = pc.GlobalCaptureKeys('http://device', 'pc', 'a' * 64)
    keys.targets = [dict(task_id='one', app='demo', channel_id=0), dict(task_id='two', app='demo', channel_id=3)]
    keys._last_seen = time.monotonic()
    done = threading.Event()
    calls = []
    class Connection:
        def __init__(self, *args): pass
        def request(self, action, **kwargs):
            target = next(t for t in keys.targets if t['task_id'] == kwargs['task_id'])
            calls.append(target['task_id']); done.set()
            return {'binding':dict(target, id='binding-' + target['task_id']), 'targets':keys.targets}
        def close(self): pass
    monkeypatch.setattr(pc, 'ControlConnection', Connection)
    worker = threading.Thread(target=keys._commands); worker.start()
    try:
        assert not keys.trigger('capture')
        for expected in ('one', 'two', 'one'):
            done.clear()
            assert keys.trigger('select')
            assert done.wait(.5)
            deadline = time.monotonic() + .5
            while keys._busy.locked() and time.monotonic() < deadline: time.sleep(.001)
            assert keys.binding['task_id'] == expected
        assert calls == ['one', 'two', 'one']
    finally:
        keys.stop(); worker.join(2)
