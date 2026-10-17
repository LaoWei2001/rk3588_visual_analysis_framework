from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import subprocess
import time
import sys
from types import SimpleNamespace

import pytest

from rk3588_runtime import recovery_history as history, recovery_observer as observer, diagnostic_retention, log_buffer


def event(category='system', **kwargs):
    return history.record(category, 'test', '故障原因', '恢复动作', fault_recovery=True, **kwargs)


def test_count_and_serialized_size_are_bounded_for_all_categories():
    for category in history.CATEGORIES:
        for index in range(26):
            event(category, event_id=f'{category}-{index}', detail='日志\x00\n' * 4000, evidence='证据' * 6000)
    snapshot = history.snapshot()
    assert len(snapshot['events']) == 60
    for category in history.CATEGORIES:
        rows = history.snapshot(category)['events']
        assert len(rows) == 20
        assert rows[0]['id'] == f'{category}-25' and rows[-1]['id'] == f'{category}-6'
    assert history.HISTORY_FILE.stat().st_size < history.MAX_FILE_BYTES
    assert len(list(history.HISTORY_FILE.parent.iterdir())) == 2  # One JSON, one fixed lock; no archive files.


def test_concurrent_producers_do_not_lose_retained_events():
    with ThreadPoolExecutor(max_workers=6) as pool:
        list(pool.map(lambda index: event(event_id=str(index)), range(20)))
    assert {row['id'] for row in history.snapshot()['events']} == {str(index) for index in range(20)}


def test_result_updates_are_durable_and_idempotent():
    key = event(event_id='same')
    event(event_id='same')
    history.update(key, 'failed', '重启命令执行失败')
    value = json.loads(history.HISTORY_FILE.read_text())
    assert len(value['events']) == 1
    assert value['events'][0]['result'] == 'failed'
    assert value['events'][0]['detail'] == '重启命令执行失败'


def test_failed_atomic_replace_preserves_previous_records(monkeypatch):
    event(event_id='original')
    original = history.HISTORY_FILE.read_bytes()
    replace = Path.replace

    def fail(path, target):
        if target == history.HISTORY_FILE:
            raise OSError('test disk failure')
        return replace(path, target)

    monkeypatch.setattr(Path, 'replace', fail)
    with pytest.raises(OSError):
        event(event_id='next')
    assert history.HISTORY_FILE.read_bytes() == original
    assert not list(history.HISTORY_FILE.parent.glob('.recovery-history-*'))


def test_corrupted_history_is_reported_instead_of_replaced():
    history.HISTORY_FILE.parent.mkdir(parents=True)
    history.HISTORY_FILE.write_text('{broken')
    assert history.snapshot()['error']
    with pytest.raises(ValueError):
        event()
    assert history.HISTORY_FILE.read_text() == '{broken'


def test_routine_operations_cannot_evict_fault_recovery_history():
    for index in range(22):
        history.record('system', 'system_boot', '内核崩溃', '整机重启', event_id=f'fault-{index}', fault_recovery=True)
    for index in range(50):
        history.record('system', 'service_replaced', '普通服务重启', '重新启动服务', event_id=f'normal-{index}')
    rows = history.snapshot('system')['events']
    faults = [row for row in rows if row['fault_recovery']]
    operations = [row for row in rows if not row['fault_recovery']]
    assert len(faults) == 20 and len(operations) == history.KEEP_OPERATIONS
    assert faults[0]['id'] == 'fault-21' and faults[-1]['id'] == 'fault-2'
    assert operations[0]['id'] == 'normal-49' and operations[-1]['id'] == 'normal-30'


def test_operation_retention_is_independent_and_shared_across_categories():
    for category in history.CATEGORIES:
        for index in range(20):
            event(category, event_id=f'fault-{category}-{index}')
    for index in range(35):
        history.record(history.CATEGORIES[index % 3], 'manual_operation', '常规操作', '执行命令',
                       event_id=f'operation-{index}', fault_recovery=False)
    snapshot = history.snapshot()
    assert snapshot['keep_operations'] == 20 and len(snapshot['events']) == 80
    operations = [row for row in snapshot['events'] if not row['fault_recovery']]
    assert len(operations) == 20
    assert operations[0]['id'] == 'operation-34' and operations[-1]['id'] == 'operation-15'
    assert all(len([row for row in snapshot['events'] if row['category'] == category and row['fault_recovery']]) == 20
               for category in history.CATEGORIES)


def test_byte_pressure_evicts_operations_before_faults(monkeypatch):
    event(event_id='fault')
    baseline = history.HISTORY_FILE.stat().st_size
    monkeypatch.setattr(history, 'MAX_FILE_BYTES', baseline + 100)
    history.record('system', 'maintenance', '普通操作' * 100, '执行操作', event_id='operation')
    assert [row['id'] for row in history.snapshot()['events']] == ['fault']
    assert history.HISTORY_FILE.stat().st_size <= history.MAX_FILE_BYTES


def test_shutdown_audit_ignores_service_restart_and_is_idempotent(monkeypatch):
    monkeypatch.setattr(sys, 'argv', ['recovery_observer', '--shutdown'])
    monkeypatch.setattr(observer.subprocess, 'run', lambda *args, **kwargs: SimpleNamespace(stdout='running\n'))
    observer.main()
    assert not history.snapshot()['events']
    monkeypatch.setattr(observer.subprocess, 'run', lambda *args, **kwargs: SimpleNamespace(stdout='stopping\n'))
    observer.main()
    observer.main()
    rows = history.snapshot()['events']
    assert len(rows) == 1 and rows[0]['kind'] == 'system_shutdown'
    assert not rows[0]['fault_recovery'] and '尚未确认' in rows[0]['detail']


@pytest.mark.parametrize('event,expected', [
    ({'category': 'system', 'kind': 'service_failure'}, False),
    ({'category': 'system', 'kind': 'service_replaced'}, False),
    ({'category': 'system', 'kind': 'system_boot', 'reason': '到达每日定时重启时间'}, False),
    ({'category': 'system', 'kind': 'system_boot', 'reason': '上一运行周期进入正常关机流程'}, False),
    ({'category': 'system', 'kind': 'system_boot', 'reason': '原因未确认'}, True),
    ({'category': 'network', 'kind': 'manual_network_config'}, False),
    ({'category': 'network', 'kind': 'reconnect'}, True),
    ({'category': 'application', 'kind': 'manual_start'}, False),
    ({'category': 'application', 'kind': 'restored', 'fault_recovery': False}, False),
    ({'category': 'application', 'kind': 'restored', 'fault_recovery': True}, True),
    ({'category': 'application', 'kind': 'service_failure'}, True),
])
def test_fault_recovery_classification_handles_legacy_and_explicit_records(event, expected):
    assert history.is_fault_recovery(event) is expected


@pytest.mark.parametrize('request_category,direct_fault,expected_category', [
    ('network', False, 'network'), ('network', True, 'system'), ('system', False, 'system'),
])
def test_boot_follows_trigger_category_unless_kernel_failure_is_confirmed(monkeypatch, request_category, direct_fault, expected_category):
    monkeypatch.setattr(history, 'boot_id', lambda: 'old')
    history.metadata({'boot_id': 'old'})
    history.request_reboot('检测到持续故障', '整机重启', category=request_category)
    monkeypatch.setattr(history, 'boot_id', lambda: 'new')
    monkeypatch.setattr(observer, 'boot_evidence', lambda *args: {
        'panic': direct_fault, 'card_reset': False, 'software_reboot': False,
        'evidence': '', 'seen': [], 'errors': []})
    observer.observe_boot()
    row = next(row for row in history.snapshot()['events'] if row['id'] == 'boot:new')
    assert row['category'] == expected_category and row['fault_recovery']


def test_scheduled_and_clean_boots_are_not_fault_recovery(monkeypatch):
    monkeypatch.setattr(history, 'boot_id', lambda: 'old')
    history.metadata({'boot_id': 'old'})
    history.record('system', 'board_reboot_request', '到达每日定时重启时间', '定时重启', fault_recovery=False)
    monkeypatch.setattr(history, 'boot_id', lambda: 'new')
    monkeypatch.setattr(observer, 'boot_evidence', lambda *args: {
        'panic': False, 'card_reset': False, 'software_reboot': False,
        'evidence': '', 'seen': [], 'errors': []})
    observer.observe_boot()
    assert not next(row for row in history.snapshot()['events'] if row['id'] == 'boot:new')['fault_recovery']
    history.metadata({'shutdown_boot_id': 'new'})
    monkeypatch.setattr(history, 'boot_id', lambda: 'third')
    observer.observe_boot()
    assert not next(row for row in history.snapshot()['events'] if row['id'] == 'boot:third')['fault_recovery']


def test_new_boot_completes_reboot_request_without_claiming_fault_is_fixed(monkeypatch):
    monkeypatch.setattr(history, 'boot_id', lambda: 'old')
    key = history.request_reboot('网络失联', '整机重启', category='network')
    history.finish_previous_boot('new', 'old')
    row = history.snapshot()['events'][0]
    assert row['id'] == key and row['result'] == 'performed'
    assert row['recovered_boot_id'] == 'new'
    assert '另行检查' in row['detail']


def normal_reboot(monkeypatch, *, fault=False):
    monkeypatch.setattr(history, 'boot_id', lambda: 'old')
    history.metadata({'boot_id': 'old', 'shutdown_boot_id': 'old'})
    request = history.record('system', 'board_reboot_request', '到达每日定时重启时间',
                             'systemd 定时任务请求整机重启', source='每日定时重启', fault_recovery=False,
                             timestamp=100)
    history.record('system', 'system_shutdown', '系统进入正常关机流程，具体发起原因未确认',
                   '系统正在结束服务并准备关机或重启', result='performed', event_id='shutdown:old',
                   fault_recovery=False, timestamp=101)
    monkeypatch.setattr(history, 'boot_id', lambda: 'new')
    monkeypatch.setattr(observer, 'boot_evidence', lambda *args: {
        'panic': fault, 'card_reset': False, 'software_reboot': True,
        'evidence': 'reboot: Restarting system', 'seen': [], 'errors': []})
    observer.observe_boot()
    return request


def test_normal_reboot_shows_one_record_with_three_stages_and_preserves_raw_audit(monkeypatch):
    request = normal_reboot(monkeypatch)
    history.record('system', 'performance_settings', '开机恢复已保存的性能参数', '应用性能设置',
                   result='performed', fault_recovery=False)
    original = history.HISTORY_FILE.read_bytes()
    snapshot = history.snapshot()
    assert len(snapshot['events']) == 2
    row = next(row for row in snapshot['events'] if row['kind'] == 'system_boot')
    assert row['reason'] == '到达每日定时重启时间' and not row['fault_recovery']
    assert row['time'] == 100 and row['source'] == '每日定时重启'
    assert row['result'] == 'performed' and '正常重启已完成' in row['detail']
    assert [stage['kind'] for stage in row['stages']] == ['board_reboot_request', 'system_shutdown', 'system_boot']
    assert row['stages'][0]['id'] == request
    assert row['previous_boot_id'] == 'old' and row['reboot_request_id'] == request
    assert len(json.loads(original)['events']) == 4
    assert history.HISTORY_FILE.read_bytes() == original
    observer.observe_boot()
    assert history.snapshot()['events'] == snapshot['events']


def test_legacy_normal_reboot_is_grouped_using_existing_links_without_writing_history(monkeypatch):
    request = normal_reboot(monkeypatch)
    value = json.loads(history.HISTORY_FILE.read_text())
    boot = next(row for row in value['events'] if row['id'] == 'boot:new')
    del boot['previous_boot_id'], boot['reboot_request_id']
    value['meta'] = {}  # The request's recovered_boot_id also works for older boots.
    history.HISTORY_FILE.write_text(json.dumps(value))
    original = history.HISTORY_FILE.read_bytes()
    rows = history.snapshot()['events']
    assert len(rows) == 1 and rows[0]['stages'][0]['id'] == request
    assert len(rows[0]['stages']) == 3
    assert history.HISTORY_FILE.read_bytes() == original


def test_time_proximity_does_not_merge_failed_or_unrelated_reboot_requests(monkeypatch):
    normal_reboot(monkeypatch)
    monkeypatch.setattr(history, 'boot_id', lambda: 'old')
    failed = history.record('system', 'board_reboot_request', '到达每日定时重启时间',
                            'systemd 定时任务请求整机重启', result='failed', fault_recovery=False,
                            timestamp=100)
    unrelated = history.record('system', 'board_reboot_request', '到达每日定时重启时间',
                               'systemd 定时任务请求整机重启', fault_recovery=False, timestamp=100)
    rows = history.snapshot()['events']
    assert len(rows) == 3
    assert {row['id'] for row in rows} == {'boot:new', failed, unrelated}
    assert all('stages' not in row for row in rows if row['id'] != 'boot:new')


def test_kernel_fault_during_scheduled_reboot_stays_visible_as_fault(monkeypatch):
    request = normal_reboot(monkeypatch, fault=True)
    rows = history.snapshot()['events']
    assert len(rows) == 3 and all('stages' not in row for row in rows)
    assert next(row for row in rows if row['id'] == 'boot:new')['fault_recovery']
    assert next(row for row in rows if row['id'] == request)['fault_recovery'] is False


def test_later_boot_does_not_relink_completed_or_stale_reboot_requests(monkeypatch):
    request = normal_reboot(monkeypatch)
    monkeypatch.setattr(history, 'boot_id', lambda: 'ancient')
    stale = history.request_reboot('旧请求', '重启')
    history.finish_previous_boot('third', 'new')
    raw = {row['id']: row for row in json.loads(history.HISTORY_FILE.read_text())['events']}
    assert raw[request]['recovered_boot_id'] == 'new'
    assert 'recovered_boot_id' not in raw[stale]
    history.finish_previous_boot('fourth', None)
    assert json.loads(history.HISTORY_FILE.read_text())['events'] == list(raw.values())


def test_normal_boot_stages_remain_linked_when_wall_clock_moves_backwards(monkeypatch):
    normal_reboot(monkeypatch)
    value = json.loads(history.HISTORY_FILE.read_text())
    next(row for row in value['events'] if row['kind'] == 'system_boot')['time'] = 1
    history.HISTORY_FILE.write_text(json.dumps(value))
    rows = history.snapshot()['events']
    assert len(rows) == 1 and rows[0]['time'] == 100
    assert [stage['time'] for stage in rows[0]['stages']] == [100, 101, 1]


def test_clean_boot_groups_shutdown_without_borrowing_an_unrelated_old_timer_request(monkeypatch):
    request = normal_reboot(monkeypatch)
    monkeypatch.setattr(history, 'boot_id', lambda: 'new')
    history.record('system', 'system_shutdown', '正常关机', '结束服务', result='performed',
                   event_id='shutdown:new', fault_recovery=False)
    history.metadata({'shutdown_boot_id': 'new'})
    monkeypatch.setattr(history, 'boot_id', lambda: 'third')
    observer.observe_boot()
    rows = history.snapshot()['events']
    assert len(rows) == 2
    current = next(row for row in rows if row['id'] == 'boot:third')
    earlier = next(row for row in rows if row['id'] == 'boot:new')
    assert '正常关机' in current['reason'] and not current['fault_recovery']
    assert [stage['id'] for stage in current['stages']] == ['shutdown:new', 'boot:third']
    assert earlier['stages'][0]['id'] == request


@pytest.mark.parametrize('evidence,reason,action', [
    ({'card_reset': True, 'panic': False, 'software_reboot': False}, '硬件看门狗', '硬件看门狗复位'),
    ({'card_reset': False, 'panic': True, 'software_reboot': True}, 'panic', '软件重启'),
    ({'card_reset': False, 'panic': True, 'software_reboot': False}, 'panic', '复位来源证据'),
    ({'card_reset': False, 'panic': False, 'software_reboot': False}, '原因未确认', '方式未确认'),
])
def test_boot_attribution_uses_evidence_without_guessing(evidence, reason, action):
    actual_reason, actual_action, _ = observer.describe_boot(evidence)
    assert reason in actual_reason and action in actual_action


def test_old_pstore_is_not_reused_and_bootstatus_is_read_without_opening_watchdog(tmp_path, monkeypatch):
    store = tmp_path / 'pstore'
    store.mkdir()
    crash = store / 'console-ramoops-0'
    crash.write_text('Kernel panic - not syncing: crash\n')
    watchdog = tmp_path / 'watchdog/watchdog7'
    watchdog.mkdir(parents=True)
    (watchdog / 'bootstatus').write_text('32')
    monkeypatch.setattr(observer, 'PSTORE_DIRS', (store,))
    monkeypatch.setattr(observer, 'WATCHDOG_SYSFS', watchdog.parent)
    first = observer.boot_evidence(None, [])
    assert first['panic'] and first['card_reset'] and first['seen']
    second = observer.boot_evidence(None, first['seen'])
    assert not second['panic']
    os.utime(crash, (0, 0))
    assert not observer.boot_evidence(None, [])['panic']


def test_boot_capture_survives_observer_restarts_without_duplicate_rows(monkeypatch):
    current = {'boot': 'first'}
    monkeypatch.setattr(history, 'boot_id', lambda: current['boot'])
    monkeypatch.setattr(observer, 'boot_evidence', lambda *args: {
        'panic': False, 'card_reset': False, 'software_reboot': False, 'evidence': '', 'seen': [], 'errors': []})
    observer.observe_boot()
    observer.observe_boot()
    assert len(history.snapshot()['events']) == 1
    current['boot'] = 'second'
    observer.observe_boot()
    assert len(history.snapshot()['events']) == 2
    assert '原因未确认' in history.snapshot()['events'][0]['reason']


def test_auto_restart_cause_and_result_are_captured_once(monkeypatch):
    unit = 'rk3588-console.service'
    status = {'count': 2, 'pid': 456, 'active': 'active'}

    def command(args):
        if 'list-units' in args:
            return unit + ' loaded active running Console\n'
        return f"Id={unit}\nNRestarts={status['count']}\nMainPID={status['pid']}\nActiveState={status['active']}\nResult=success\n"

    monkeypatch.setattr(observer, 'command', command)
    monkeypatch.setattr(observer, 'journal', lambda *args: "service: Failed with result 'watchdog'.")
    observer.observe_services()
    observer.observe_services()
    rows = history.snapshot()['events']
    assert len(rows) == 1 and '心跳' in rows[0]['reason'] and rows[0]['result'] == 'performed'
    status.update(count=0, pid=789)
    observer.observe_services()
    assert len(history.snapshot()['events']) == 2
    assert '发起原因未确认' in history.snapshot()['events'][0]['reason']


def test_native_service_hooks_capture_each_crash_and_rotate_without_polling(monkeypatch):
    unit = 'rk3588-app-demo.service'
    monkeypatch.setenv('SERVICE_RESULT', 'watchdog')
    monkeypatch.setenv('EXIT_CODE', 'killed')
    monkeypatch.setenv('EXIT_STATUS', 'KILL')
    for index in range(25):
        monkeypatch.setenv('INVOCATION_ID', str(index))
        observer.record_service_exit(unit)
        assert history.snapshot('application')['events'][0]['result'] == 'pending'
        observer.complete_service_start(unit)
    events = history.snapshot('application')['events']
    assert len(events) == 20
    assert all(event['result'] == 'performed' and '心跳' in event['reason'] for event in events)
    assert all('systemd' in event['action'] and 'EXIT_STATUS=KILL' in event['evidence'] for event in events)
    assert events[0]['id'].startswith('exit:24:') and events[-1]['id'].startswith('exit:5:')


def test_polling_does_not_duplicate_native_service_recovery_records(monkeypatch):
    unit = 'rk3588-console.service'
    monkeypatch.setenv('SERVICE_RESULT', 'exit-code')
    observer.record_service_exit(unit)
    observer.complete_service_start(unit)
    monkeypatch.setattr(observer, 'command', lambda args:
        unit + ' loaded active running Console\n' if 'list-units' in args
        else f'Id={unit}\nNRestarts=1\nMainPID=333\nActiveState=active\nResult=success\n')
    observer.observe_services()
    assert len(history.snapshot()['events']) == 1


def test_diagnostic_cleanup_only_removes_named_archives_and_verified_core_dumps(tmp_path):
    store = tmp_path / 'pstore'
    store.mkdir()
    for index in range(7):
        (store / f'console-ramoops-{index}').write_text('crash')
    (store / 'other-config').write_text('keep')
    app = tmp_path / 'apps/demo'
    app.mkdir(parents=True)
    (app / 'core-not-a-dump').write_text('business configuration')
    header = bytearray(18)
    header[:4] = b'\x7fELF'; header[5] = 1; header[16] = 4
    for index in range(7):
        (app / f'core.{index}').write_bytes(header)
    media = app / 'assets'
    media.mkdir()
    (media / 'capture.mp4').write_bytes(b'media')
    assert diagnostic_retention.cleanup(store, tmp_path / 'apps') == 8
    assert len(list(store.glob('console-*'))) == 3
    assert len(list(app.glob('core.[0-9]'))) == 3
    assert (store / 'other-config').exists() and (app / 'core-not-a-dump').exists()
    assert (media / 'capture.mp4').exists()


def test_slow_subscriber_and_stalled_event_loop_cannot_accumulate_unbounded_logs():
    import asyncio

    async def exercise():
        buffer = log_buffer.AppLogBuffer()
        queue = asyncio.Queue(maxsize=log_buffer.MAX_SUBSCRIBER_LINES)
        buffer.subscribe(queue)
        for index in range(20000):
            buffer.push(str(index) + 'x' * 10000)
        assert len(buffer._lines) == log_buffer.MAX_LINES
        assert len(buffer._pending) <= log_buffer.MAX_SUBSCRIBER_LINES
        await asyncio.sleep(0)
        assert queue.qsize() == log_buffer.MAX_SUBSCRIBER_LINES
        assert len(buffer.get_tail(1)[0]) <= log_buffer.MAX_LINE_CHARS
        assert buffer.get_tail(1)[0].startswith('19999')
        buffer.unsubscribe(queue)

    __import__('asyncio').run(exercise())
