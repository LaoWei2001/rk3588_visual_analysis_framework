import json
from pathlib import Path
import time
import sys

import pytest

from rk3588_runtime import fault_diagnostics as fault, recovery_observer as observer, recovery_history as history


@pytest.mark.parametrize('line,code', [
    ('lkdtm: Performing direct entry HARDLOCKUP', 'hardlockup_test'),
    ('Kernel panic - not syncing: Fatal exception', 'panic'),
    ('oom-kill:constraint=CONSTRAINT_NONE,pid=123', 'oom'),
    ('watchdog: BUG: soft lockup - CPU#3 stuck for 26s', 'lockup'),
    ('rcu: INFO: rcu_preempt detected stalls on CPUs/tasks', 'lockup'),
    ('INFO: task main:321 blocked for more than 120 seconds.', 'blocked'),
    ('Unable to handle kernel NULL pointer dereference', 'kernel_fault'),
    ('RKNPU fdab0000.npu: job timeout', 'npu'),
    ('mali fb000000.gpu: GPU fault 0x40', 'gpu'),
    ('rkvdec2 fdc38100.rkvdec: task timeout', 'codec'),
    ('rk_iommu fdc38700.iommu: Page fault at 0x123', 'iommu'),
    ('mmc0: Timeout waiting for hardware interrupt.', 'storage'),
    ('EXT4-fs error (device mmcblk0p8): corrupted inode', 'storage'),
    ('write: No space left on device', 'disk_full'),
    ('thermal thermal_zone0: critical temperature reached', 'thermal'),
    ('PMIC: under-voltage detected', 'power'),
    ('NETDEV WATCHDOG: eth2 (r8125): transmit queue timed out', 'network_driver'),
    ('xhci-hcd: host controller not responding, assume dead', 'usb'),
])
def test_multiple_fault_families_have_original_evidence(line, code):
    found = fault.signals('[ 123.0] ' + line, 'test-kernel', 'boot', end=130)
    assert any(item['code'] == code and line in item['line'] and item['source'] == 'test-kernel' for item in found)


def test_old_faults_and_normal_driver_initialization_do_not_explain_new_reboot():
    text = '[ 1.0] RKNPU initialized successfully\n[ 2.0] mali: timeout\n[ 1000.0] normal activity'
    assert fault.signals(text, 'pstore', 'old') == []
    assert fault.signals('[ 1.0] RKNPU: timeout', 'journal', 'old', end=1000) == []
    assert fault.signals('[ 9.0] normal kernel started', 'journal', 'old') == []


def test_manual_request_wins_over_correlated_driver_symptom():
    evidence = {'panic': False, 'card_reset': False, 'software_reboot': False,
                'findings': fault.signals('[ 90.0] RKNPU: job timeout', 'pstore', 'old')}
    requested = {'reason': '用户请求重启', 'action': '通过界面重启'}
    assert observer.describe_boot(evidence, requested)[0] == requested['reason']
    reason, action, detail = observer.describe_boot(evidence)
    assert 'NPU' in reason and '因果关系待确认' in reason and '根因' in detail
    assert '方式未确认' in action


def test_missing_vendor_interfaces_degrade_independently(tmp_path, monkeypatch):
    monkeypatch.setattr(fault, 'PROC', tmp_path / 'proc')
    monkeypatch.setattr(fault, 'SYS', tmp_path / 'sys')
    proc = fault.PROC
    proc.mkdir()
    (proc / 'meminfo').write_text('MemTotal: 1000 kB\nMemAvailable: 20 kB\nSwapTotal: 0 kB\n')
    (proc / 'loadavg').write_text('8.0 4.0 1.0 1/99 100')
    sample = fault.resources('boot')
    assert sample['memory_kib']['MemAvailable'] == 20
    assert '不能据此确认 OOM' in sample['alerts'][0]
    assert sample['pressure'] == {} and sample['thermal'] == []
    capabilities = fault.board_capabilities()
    assert capabilities['reset_sources'] == []
    assert any('bootstatus' in item for item in capabilities['limitations'])


def test_vendor_temperature_trip_and_reset_code_are_not_hardcoded(tmp_path, monkeypatch):
    monkeypatch.setattr(fault, 'SYS', tmp_path)
    zone = tmp_path / 'class/thermal/thermal_zone5'
    zone.mkdir(parents=True)
    for name, content in {'type': 'vendor-soc', 'temp': '90000', 'trip_point_0_type': 'critical', 'trip_point_0_temp': '85000'}.items():
        (zone / name).write_text(content)
    reason = tmp_path / 'kernel/reset_reason'
    reason.parent.mkdir()
    reason.write_text('0x32')
    sample = fault.resources('boot')
    assert any('临界温度' in alert for alert in sample['alerts'])
    assert sample['thermal'][0]['critical_millidegrees'] == 85000
    board = fault.board_capabilities()
    assert board['reset_sources'][0]['value'] == '0x32'
    assert not any('watchdog' in item for item in board['reset_sources'][0].values())


def test_samples_signals_and_disk_usage_remain_bounded(monkeypatch):
    now = {'n': 0}
    monkeypatch.setattr(fault.time, 'monotonic', lambda: now['n'] * 61.)
    monkeypatch.setattr(fault, 'board_capabilities', lambda: {'model': 'vendor board'})
    monkeypatch.setattr(fault, 'bounded_command', lambda args: f"[ {now['n'] * 61}.0] RKNPU: job timeout {now['n']}")
    for n in range(30):
        now['n'] = n
        fault.collect('old')
    value = fault.load()
    assert len(value['samples']) == 3 and len(value['signals']) == 20
    assert fault.STORE.stat().st_size <= fault.MAX_BYTES
    assert not list(fault.STORE.parent.glob('.recovery-diagnostics-*'))
    assert len(fault.previous_context('old')[0]) == 3
    assert fault.previous_context('unrelated') == ([], [])


def test_upgrade_captures_protection_immediately_then_respects_sampling_interval(monkeypatch):
    monkeypatch.setattr(fault.time, 'monotonic', lambda: 100.)
    monkeypatch.setattr(fault, 'board_capabilities', lambda: {})
    monkeypatch.setattr(fault, 'resources', lambda *_: {'boot_id': 'boot', 'uptime': 100., 'time': 1000.})
    monkeypatch.setattr(fault, 'recovery_state', lambda: {'panic_timeout_sec': 0})
    monkeypatch.setattr(fault, 'bounded_command', lambda *_: '')
    fault.STORE.parent.mkdir(parents=True)
    fault.STORE.write_text(json.dumps({'version': 1, 'samples': [{'boot_id': 'boot', 'uptime': 99.}], 'signals': []}))
    fault.collect('boot')
    assert fault.load()['samples'][-1]['recovery_state']['panic_timeout_sec'] == 0
    original = fault.STORE.read_bytes()
    fault.collect('boot')
    assert fault.STORE.read_bytes() == original


def test_persisted_faults_require_recent_matching_boot():
    fault.STORE.parent.mkdir(parents=True)
    fault.STORE.write_text(json.dumps({'version': 1, 'samples': [{'boot_id': 'old', 'uptime': 1000}],
                                    'signals': [{'boot_id': 'old', 'uptime': 5}, {'boot_id': 'other', 'uptime': 999},
                                                {'boot_id': 'old', 'uptime': 990}]}))
    _, found = fault.previous_context('old')
    assert found == [{'boot_id': 'old', 'uptime': 990}]


def test_sustained_memory_pressure_is_not_misreported_as_oom_or_power_cut():
    sample = {'alerts': ['可用内存低于 3%（不能据此确认 OOM）']}
    reason, _, detail = observer.describe_boot({'panic': False, 'card_reset': False, 'samples': [sample, sample]})
    assert '持续出现' in reason and '不能据此确认 OOM' in reason
    assert '尚无证据区分' in detail


def test_diagnostic_metadata_cannot_break_history_byte_limit():
    for category in history.CATEGORIES:
        for n in range(21):
            history.record(category, 'test', '原因' * 1000, '动作' * 1000, detail='\x00' * 4000,
                           evidence='\x00' * 4000,
                           diagnosis={'confidence': 'probable', 'findings': [{'code': 'x' * 1000, 'summary': '\x00' * 1000}] * 10,
                                      'limitations': ['\x00' * 1000] * 10})
    assert history.HISTORY_FILE.stat().st_size <= history.MAX_FILE_BYTES
    for row in history.snapshot()['events']:
        assert len(json.dumps(row['diagnosis'], ensure_ascii=False).encode()) <= 1024


def test_corrupt_diagnostic_store_is_reported_without_losing_recovery_events():
    fault.STORE.parent.mkdir(parents=True)
    fault.STORE.write_text('{broken')
    history.record('system', 'test', '原因', '动作')
    snapshot = history.snapshot()
    assert len(snapshot['events']) == 1 and not snapshot['error']
    assert snapshot['diagnostics']['error']


def test_native_signal_exit_has_precise_termination_evidence(monkeypatch):
    monkeypatch.setenv('SERVICE_RESULT', 'signal')
    monkeypatch.setenv('EXIT_STATUS', 'KILL')
    observer.record_service_exit('rk3588-console.service')
    row = history.snapshot()['events'][0]
    assert 'SIGKILL' in row['reason'] and '不能确认 OOM' in row['reason']
    assert row['diagnosis']['confidence'] == 'confirmed'


def test_evidence_command_has_bounded_memory_and_output():
    result = fault.bounded_command([sys.executable, '-c', "import sys; sys.stdout.write('x'*1000000+'tail')"])
    assert len(result.encode()) == 256 * 1024 and result.endswith('tail')
    with pytest.raises(RuntimeError, match='读取上限'):
        fault.bounded_command([sys.executable, '-c', "import sys; sys.stdout.write('x'*5000000)"])


def test_old_kernel_panic_is_not_assigned_to_later_boot(tmp_path, monkeypatch):
    monkeypatch.setattr(observer, 'PSTORE_DIRS', ())
    monkeypatch.setattr(observer, 'WATCHDOG_SYSFS', tmp_path)
    monkeypatch.setattr(observer, 'journal', lambda *_: '[ 3.0] Kernel panic - not syncing: old failure')
    monkeypatch.setattr(fault, 'bounded_command', lambda *_: '[ 9999.0] last system activity')
    evidence = observer.boot_evidence('previous', [])
    assert not evidence['panic'] and not evidence['findings']
    assert 'old failure' not in evidence['evidence']


def test_hardlockup_pstore_records_injection_count_without_claiming_cpu_count(tmp_path, monkeypatch):
    store = tmp_path / 'pstore'
    store.mkdir()
    text = '\n'.join(f'[ {100 + n / 100:.2f}] lkdtm: Performing direct entry HARDLOCKUP' for n in range(8))
    (store / 'console-ramoops-0').write_text(text)
    monkeypatch.setattr(observer, 'PSTORE_DIRS', (store,))
    monkeypatch.setattr(observer, 'WATCHDOG_SYSFS', tmp_path / 'watchdogs')
    monkeypatch.setattr(observer, 'journal', lambda *_: text)
    monkeypatch.setattr(fault, 'bounded_command', lambda *_: '[ 101.0] last activity')
    evidence = observer.boot_evidence('old', [])
    assert evidence['hardlockup_count'] == 8  # pstore + journal are not 16 injections.
    reason, action, detail = observer.describe_boot(evidence)
    assert '人为注入' in reason and '8 次' in reason
    assert '核心数' in detail and 'HARDLOCKUP' in evidence['evidence']
    assert observer.recovery_assessment(evidence)['confidence'] == 'unknown'
    assert '硬件看门狗' not in action


def protected_fault(*, panic=False, age=20, driver='dw_wdt', timeout=11):
    return {'panic': panic, 'card_reset': False, 'software_reboot': False,
            'hardlockup_count': 0 if panic else 8,
            'findings': [{'uptime': 100, 'code': 'panic' if panic else 'hardlockup_test', 'summary': '测试故障'}],
            'samples': [{'uptime': 100 - age, 'recovery_state': {
                'panic_timeout_sec': 0, 'watchdog_timeout_sec': timeout,
                'watchdog_manager_devices': ['/dev/watchdog1'], 'watchdog_drivers': [driver]}}],
            'board': {'limitations': ['看门狗未提供 bootstatus']}}


@pytest.mark.parametrize('panic', [False, True])
def test_recovery_method_can_be_inferred_but_not_confirmed_from_pre_fault_protection(panic):
    evidence = protected_fault(panic=panic)
    recovery = observer.recovery_assessment(evidence)
    assert recovery['confidence'] == 'probable' and '硬件看门狗' in recovery['method']
    assert '11 秒' in recovery['method']
    assert any('panic=0' in item for item in recovery['basis'])
    assert any('人工复位' in item for item in recovery['limitations'])
    evidence['card_reset'] = True
    assert observer.recovery_assessment(evidence)['confidence'] == 'confirmed'


@pytest.mark.parametrize('override', [
    {'age': fault.WINDOW + 1}, {'age': -1}, {'driver': 'softdog'}, {'timeout': 0},
])
def test_stale_future_disabled_or_software_watchdog_cannot_support_hardware_attribution(override):
    recovery = observer.recovery_assessment(protected_fault(**override))
    assert recovery['confidence'] == 'unknown'


def test_panic_countdown_is_not_confused_with_executing_software_restart():
    evidence = {'panic': True, 'card_reset': False, 'software_reboot': True}
    recovery = observer.recovery_assessment(evidence)
    assert recovery['confidence'] == 'unknown' and '已安排' in recovery['method']
    evidence['software_restart'] = True
    recovery = observer.recovery_assessment(evidence)
    assert recovery['confidence'] == 'probable' and '重启入口' in recovery['method']


def test_lockup_followed_by_panic_with_software_restart_enabled_is_ambiguous():
    evidence = protected_fault(panic=True)
    evidence['hardlockup_count'] = 8
    evidence['samples'][0]['recovery_state']['panic_timeout_sec'] = 1
    assert observer.recovery_assessment(evidence)['confidence'] == 'unknown'


def test_unrelated_later_symptom_cannot_make_a_post_fault_snapshot_look_valid():
    evidence = protected_fault(age=-10)
    evidence['findings'].append({'code': 'npu', 'uptime': 120})
    assert observer.recovery_assessment(evidence)['confidence'] == 'unknown'


def test_reset_assessment_survives_bounded_history_serialization():
    evidence = protected_fault()
    diagnosis = fault.diagnosis([{'code': 'hardlockup_test', 'summary': '硬锁死测试'}], confirmed=True,
                               limitations=['故障证据不等于复位来源证据'])
    diagnosis['recovery'] = observer.recovery_assessment(evidence)
    history.record('system', 'system_boot', '测试注入', '推测硬件复位', diagnosis=diagnosis)
    stored = history.snapshot()['events'][0]['diagnosis']
    assert len(json.dumps(stored, ensure_ascii=False).encode()) <= 1024
    assert stored['confidence'] == 'confirmed' and stored['recovery']['confidence'] == 'probable'
    assert stored['recovery']['basis'] and stored['recovery']['limitations']


def test_panic_reason_excludes_end_marker_and_identifies_sysrq(tmp_path, monkeypatch):
    store = tmp_path / 'pstore'
    store.mkdir()
    (store / 'console-ramoops-0').write_text(
        '[ 100.0] Kernel panic - not syncing: sysrq triggered crash\n'
        '[ 101.0] ---[ end Kernel panic - not syncing: sysrq triggered crash ]---')
    monkeypatch.setattr(observer, 'PSTORE_DIRS', (store,))
    monkeypatch.setattr(observer, 'WATCHDOG_SYSFS', tmp_path / 'watchdogs')
    evidence = observer.boot_evidence(None, [])
    assert evidence['panic_reason'] == 'sysrq triggered crash'
    assert 'SysRq 请求触发' in observer.describe_boot(evidence)[0]


def test_upgrade_refines_current_record_only_and_preserves_original_time(tmp_path, monkeypatch):
    monkeypatch.setattr(history, 'boot_id', lambda: 'new')
    old = history.record('system', 'system_boot', '旧故障', '旧方式', event_id='boot:old')
    key = history.record('system', 'system_boot', '原因未确认', '方式未确认', event_id='boot:new', timestamp=1000)
    history.metadata({'boot_id': 'new'})
    evidence = protected_fault()
    evidence.update({'evidence': 'HARDLOCKUP 原始日志', 'seen': [], 'errors': []})
    calls = []
    monkeypatch.setattr(observer, 'boot_evidence', lambda *args: calls.append(args) or evidence)
    observer.observe_boot()
    observer.observe_boot()
    rows = {row['id']: row for row in history.snapshot()['events']}
    assert len(calls) == 1
    assert rows[old]['reason'] == '旧故障'
    assert rows[key]['time'] == 1000 and 'HARDLOCKUP' in rows[key]['reason']
    assert rows[key]['diagnosis']['confidence'] == 'confirmed'
    assert rows[key]['diagnosis']['recovery']['confidence'] == 'probable'


def test_pre_fault_snapshot_reads_actual_manager_state_without_opening_watchdog(tmp_path, monkeypatch):
    monkeypatch.setattr(fault, 'PROC', tmp_path / 'proc')
    monkeypatch.setattr(fault, 'SYS', tmp_path / 'sys')
    panic = fault.PROC / 'sys/kernel/panic'
    panic.parent.mkdir(parents=True)
    panic.write_text('0')
    fds = fault.PROC / '1/fd'
    fds.mkdir(parents=True)
    (fds / '10').symlink_to('/dev/watchdog1')
    driver = fault.SYS / 'class/watchdog/watchdog1/device/driver'
    driver.parent.mkdir(parents=True)
    driver.symlink_to(tmp_path / 'drivers/dw_wdt')
    monkeypatch.setattr(fault, 'bounded_command', lambda *_: '11s\n')
    state = fault.recovery_state()
    assert state['panic_timeout_sec'] == 0 and state['watchdog_timeout_sec'] == 11
    assert state['watchdog_manager_devices'] == ['/dev/watchdog1']
    assert state['watchdog_drivers'] == ['dw_wdt'] and not state['errors']


def test_old_panic_is_refined_from_its_own_evidence_without_using_current_settings(monkeypatch):
    monkeypatch.setattr(history, 'boot_id', lambda: 'new')
    key = history.record('system', 'system_boot', '内核发生严重错误（panic）：sysrq triggered crash ]---',
                         '软件重启或硬件复位方式未确认', detail='设备已重新启动', source_boot_id='old',
                         evidence='[ 10.0] Kernel panic - not syncing: sysrq triggered crash\n'
                         '[ 11.0] ---[ end Kernel panic - not syncing: sysrq triggered crash ]---', timestamp=1000)
    monkeypatch.setattr(fault, 'recovery_state', lambda: pytest.fail('must not read current protection'))
    observer.refine_saved_panic_records('new')
    row = history.snapshot()['events'][0]
    assert row['id'] == key and row['time'] == 1000
    assert row['reason'] == 'SysRq 请求触发内核崩溃（panic）'
    assert row['diagnosis']['recovery']['confidence'] == 'unknown'
    assert any('历史记录' in item for item in row['diagnosis']['recovery']['limitations'])
