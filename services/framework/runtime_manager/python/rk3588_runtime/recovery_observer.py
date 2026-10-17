"""Observe boots and systemd restarts without opening watchdog devices."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import threading
import time

from . import recovery_history as history
from . import fault_diagnostics as diagnostics
from .service_watchdog import notify

PSTORE_DIRS = (Path('/sys/fs/pstore'), Path('/var/lib/systemd/pstore'))
WATCHDOG_SYSFS = Path('/sys/class/watchdog')
UNIT = re.compile(r'rk3588-app-[A-Za-z0-9_.-]+\.service|rk3588-(?:console|runtime-manager|network-recovery|recovery-history|gpio-control|gpio-restore)\.service|(?:ota_agent|unified_upload)\.service')
CAUSES = {'watchdog': '程序或服务未按时响应心跳，服务看门狗超时',
          'timeout': '程序或服务执行超时', 'signal': '程序或服务被信号终止',
          'core-dump': '程序或服务崩溃并产生转储', 'exit-code': '程序或服务异常退出',
          'oom-kill': '程序或服务因内存不足被终止'}
BOOT_ANALYSIS_VERSION = 2


def record_service_exit(unit):
    if not UNIT.fullmatch(unit):
        raise ValueError('不支持的项目服务')
    result = os.environ.get('SERVICE_RESULT', 'unknown')
    if result == 'success':
        return
    invocation = os.environ.get('INVOCATION_ID', '')
    exit_status = os.environ.get('EXIT_STATUS', '')
    reason = CAUSES.get(result, '服务异常结束，具体原因未确认')
    if result in ('signal', 'core-dump'):
        reason += {'SEGV': '（非法内存访问 SIGSEGV）', 'ABRT': '（进程主动中止 SIGABRT）',
                   'BUS': '（总线访问异常 SIGBUS）', 'ILL': '（非法指令 SIGILL）',
                   'KILL': '（SIGKILL；仅凭该信号不能确认 OOM）'}.get(exit_status.removeprefix('SIG'), '')
    extra = []
    try:
        samples, found = diagnostics.previous_context(history.boot_id())
        # These are contemporaneous system symptoms, not proof about this process.
        if samples and time.time() - samples[-1]['time'] <= diagnostics.WINDOW:
            extra = [f"同期系统迹象（未证明与本进程有关）：{item['summary']}；{item['line']}" for item in found[-2:]]
            if samples[-1].get('alerts'):
                extra.append('同期资源采样：' + '；'.join(samples[-1]['alerts'][:2]))
            blocked = [item for item in samples[-1].get('blocked_tasks', []) if item.get('unit') == unit]
            if blocked:
                extra.append('该服务最近采样发现 D 状态线程：' + str(blocked[:2]))
    except (OSError, ValueError, TypeError):
        pass
    history.record('application' if unit.startswith('rk3588-app-') else 'system', 'service_failure',
                   reason, f'等待系统服务机制处理 {unit}',
                   detail='服务已结束运行，等待后续启动结果。', source='systemd 服务退出通知', subject=unit,
                   event_id=f'exit:{invocation}:{unit}' if invocation else None,
                   evidence=f"SERVICE_RESULT={result}；EXIT_CODE={os.environ.get('EXIT_CODE', '')}；"
                   f"EXIT_STATUS={exit_status}；服务={unit}" + ('\n' + '\n'.join(extra) if extra else ''),
                   diagnosis=diagnostics.diagnosis(confirmed=result in CAUSES,
                       limitations=['退出状态确认终止方式；应用内部根因需结合程序日志。']))


def complete_service_start(unit):
    if not UNIT.fullmatch(unit):
        raise ValueError('不支持的项目服务')
    for event in history.snapshot()['events']:
        if (event.get('subject') == unit and event.get('kind') == 'service_failure'
                and event.get('boot_id') == history.boot_id() and event.get('result') == 'pending'):
            history.update(event['id'], 'performed', '系统服务机制已启动新进程；业务状态需另行检查。',
                           action=f'通过 systemd 重新启动 {unit}')
            break


def command(args):
    result = subprocess.run(args, capture_output=True, text=True, timeout=5,
                            env={**os.environ, 'LC_ALL': 'C'})
    if result.returncode:
        raise RuntimeError(result.stderr.strip()[-300:] or '系统证据读取失败')
    return result.stdout


def journal(boot, unit=None):
    args = ['journalctl', '--no-pager', '-o', 'cat' if unit else 'short-monotonic', '-b', boot, '-n', '1200']
    args += ['-u', unit] if unit else ['-k']
    return diagnostics.bounded_command(args)


def boot_evidence(previous_boot, seen):
    evidence = []
    errors = []
    panic = False
    panic_reason = ''
    software_reboot = False
    software_restart = False
    hardlockup_count = 0
    card_reset = False
    fingerprints = list(seen)
    texts = []
    findings = []
    # Archives from old crashes may survive multiple boots. Use both freshness
    # and content fingerprints so they cannot identify a later unrelated reboot.
    startup = time.time() - time.monotonic()
    for directory in PSTORE_DIRS:
        if not directory.exists():
            continue
        for path in sorted(directory.iterdir())[:20]:
            try:
                if path.is_symlink() or not path.is_file() or path.stat().st_mtime < startup - 60:
                    continue
                with path.open('rb') as handle:
                    handle.seek(max(0, path.stat().st_size - 256 * 1024))
                    data = handle.read(256 * 1024)
                digest = hashlib.sha256(data).hexdigest()
                if digest in seen:
                    continue
                if digest not in fingerprints:
                    fingerprints.append(digest)
                    texts.append((str(path), data.decode(errors='replace')))
            except OSError as exc:
                errors.append(str(exc)[:200])
    if previous_boot:
        try:
            kernel = journal(previous_boot)
            # The final kernel message can precede reboot by hours. Anchor to
            # the last system journal entry before interpreting it as recent.
            tail = diagnostics.bounded_command(['journalctl', '--no-pager', '-b', previous_boot,
                                                '-o', 'short-monotonic', '-n', '1'])
            texts.append(('上一运行周期内核日志', kernel + '\n' + tail))
        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
            errors.append(str(exc)[:200])
    for source, text in texts:
        parsed = diagnostics.signals(text, source, previous_boot or 'previous-unidentified')
        findings.extend(parsed)
        times = [float(match[1]) for line in text.splitlines() if (match := diagnostics.STAMP.match(line))]
        end = max(times, default=None)
        lines = [line for line in text.splitlines()
                 if re.search(r'Kernel panic - not syncing|Rebooting in \d+ seconds|reboot: Restarting system|sysrq: Trigger a crash|lkdtm: Performing direct entry HARDLOCKUP\b', line)
                 and (end is None or not (stamp := diagnostics.STAMP.match(line))
                      or 0 <= end - float(stamp[1]) <= diagnostics.WINDOW)]
        if lines:
            evidence.append(source + ': ' + '\n'.join(lines)[-1200:])
        panic_lines = [line for line in lines if 'Kernel panic - not syncing:' in line and 'end Kernel panic' not in line]
        panic |= any(item['code'] == 'panic' for item in parsed)
        matches = re.findall(r'Kernel panic - not syncing:\s*([^\n]+)',
                             '\n'.join(line for line in panic_lines if 'end Kernel panic' not in line))
        if matches:
            panic_reason = matches[-1][:240]
        software_reboot |= bool(re.search(r'Rebooting in \d+ seconds|reboot: Restarting system', '\n'.join(lines)))
        software_restart |= any('reboot: Restarting system' in line for line in lines)
        # Journal and pstore can contain the same messages. Count per source,
        # never add copies; multiple entries do not prove distinct CPUs locked.
        hardlockup_count = max(hardlockup_count, sum('lkdtm: Performing direct entry HARDLOCKUP' in line for line in lines))
    if WATCHDOG_SYSFS.exists():
        for path in sorted(WATCHDOG_SYSFS.glob('watchdog*/bootstatus'))[:16]:
            try:
                status = int(path.read_text().strip(), 0)
                if status & 0x20:  # WDIOF_CARDRESET, Linux watchdog.h
                    card_reset = True
                    evidence.append(f'{path}: {status}（硬件报告看门狗曾复位 CPU）')
            except (OSError, ValueError) as exc:
                errors.append(str(exc)[:200])
    samples = []
    try:
        samples, recent = diagnostics.previous_context(previous_boot)
        findings.extend(recent)
    except (OSError, ValueError, TypeError) as exc:
        errors.append('故障前采样读取失败：' + str(exc)[:160])
    # Latest board snapshot is detected read-only; raw vendor reset values are
    # preserved without guessing the meaning of undocumented numeric codes.
    board = diagnostics.board_capabilities()
    for item in board['reset_sources']:
        evidence.append(f"复位原因接口 {item['source']}={item['value']}（厂商原始值，需对应 BSP 解释）")
    unique = {}
    for item in findings:
        old = unique.get(item['code'])
        stamp = item.get('uptime')
        old_stamp = old.get('uptime') if old else None
        if old is None or (stamp is not None and (old_stamp is None or stamp >= old_stamp)):
            unique[item['code']] = item
    findings = list(unique.values())[-20:]
    for item in findings[-3:]:
        excerpt = item['line'].encode()[-240:].decode(errors='ignore')
        evidence.append(f"{item['summary']} [{item['source']}] {excerpt}")
    if samples:
        last = samples[-1]
        evidence.append(f"故障前最近采样：运行 {last['uptime']:.0f} 秒；时间戳 {last['time']:.0f}；"
                        f"内存 KiB={last.get('memory_kib', {})}；负载={last.get('load', '')}；"
                        f"告警={last.get('alerts', [])[:2]}；阻塞任务数={len(last.get('blocked_tasks', []))}")
    if errors:
        evidence.append('证据读取限制：' + '；'.join(errors[:2]))
    return {'panic': panic, 'panic_reason': panic_reason, 'software_reboot': software_reboot,
            'software_restart': software_restart, 'hardlockup_count': hardlockup_count, 'card_reset': card_reset,
            'evidence': '\n'.join(evidence).encode()[:1800].decode(errors='ignore'), 'errors': errors[:8], 'seen': fingerprints[-20:],
            'findings': findings, 'samples': samples, 'board': board}


def recovery_assessment(evidence, requested=None, clean=False):
    """Keep confirmed fault evidence separate from the final reset attribution."""
    basis = []
    state = {}
    samples = evidence.get('samples', [])
    times = [item['uptime'] for item in evidence.get('findings', [])
             if item.get('code') in ('panic', 'hardlockup_test') and item.get('uptime') is not None]
    if samples and times:
        last = samples[-1]
        age = max(times) - last.get('uptime', float('-inf'))
        if 0 <= age <= diagnostics.WINDOW:
            state = last.get('recovery_state', {})
            if 'panic_timeout_sec' in state:
                basis.append(f"故障前约 {age:.0f} 秒采样：kernel.panic={state['panic_timeout_sec']}（不是故障瞬间值）")
    watchdog = (state.get('watchdog_timeout_sec', 0) > 0 and state.get('watchdog_manager_devices')
                and set(state.get('watchdog_drivers', [])) & {'dw_wdt', 'gpio-wdt', 'gpio_wdt'})
    timeout = state.get('watchdog_timeout_sec')
    if watchdog:
        basis.append(f"同次采样：PID 1 持有硬件看门狗，实际超时 {timeout} 秒")
    limitations = []
    if evidence.get('card_reset'):
        confidence, method = 'confirmed', '硬件看门狗超时复位'
        basis.insert(0, '看门狗 bootstatus 包含 WDIOF_CARDRESET 标志')
    elif evidence.get('software_restart'):
        confidence, method = 'probable', '软件重启（内核已执行到重启入口）'
        basis.insert(0, '故障末期日志出现 reboot: Restarting system')
        limitations.append('进入软件重启流程后仍可能卡住并由硬件接管，不能仅凭该日志确认最终复位源。')
    elif watchdog and ((evidence.get('hardlockup_count') and not evidence.get('panic'))
                       or (evidence.get('panic') and state.get('panic_timeout_sec') == 0)):
        confidence, method = 'probable', f'硬件看门狗超时复位（采样超时 {timeout} 秒）'
        basis.insert(0, '故障前硬件看门狗已运行；' + ('发现 HARDLOCKUP 测试注入记录' if evidence.get('hardlockup_count')
                                                              else '发现 panic，最近采样的 panic 自动重启设置为关闭'))
        limitations.append('故障前采样与实际故障之间仍可能有设置变化；无法排除人工复位或重新上电。')
    elif evidence.get('software_reboot'):
        confidence, method = 'unknown', '内核已安排软件重启，执行结果缺少证据'
        basis.insert(0, '内核日志留下重启倒计时；没有实际进入重启入口的日志')
    elif requested:
        confidence, method = 'probable', requested['action']
        basis.insert(0, '上次启动保存了重启请求，本次已观察到新的启动 ID')
        limitations.append('重启请求只能确认预定执行方式，不能证明最终复位源。')
    elif clean:
        confidence, method = 'unknown', '正常关机流程后的启动'
        basis.append('上次运行保存了正常关机标记')
    else:
        confidence, method = 'unknown', '已重新启动，尚缺复位来源证据'
        if evidence.get('hardlockup_count'):
            basis.append(f"持久日志记录 {evidence['hardlockup_count']} 次 HARDLOCKUP 注入；不能仅凭条数确认锁死核心数")
        elif evidence.get('panic'):
            basis.append('持久日志确认发生 panic，但未记录实际进入重启入口')
        if not state:
            limitations.append('未取得足够接近故障时刻的保护状态采样，不能用本次启动后的设置倒推。')
    if not evidence.get('card_reset'):
        limitations.extend(item for item in evidence.get('board', {}).get('limitations', [])
                           if '复位原因' in item or 'bootstatus' in item)
        if not limitations:
            limitations.append('缺少能归属于本次启动的硬件复位原因。')
    return {'confidence': confidence, 'method': method, 'basis': basis[:3], 'limitations': limitations[:3]}


def describe_boot(evidence, requested=None, clean=False):
    if evidence['card_reset']:
        return ('硬件看门狗报告超时复位' + ('，同时发现内核崩溃证据' if evidence['panic'] else '；停止喂狗的根因未确认'),
                '硬件看门狗复位整机，设备重新启动', '硬件状态已确认；业务和网络是否恢复需另行检查。')
    assessment = recovery_assessment(evidence, requested, clean)
    if evidence.get('hardlockup_count'):
        return (f"人为注入 CPU 硬锁死测试（HARDLOCKUP，日志记录 {evidence['hardlockup_count']} 次）",
                ('推测：' if assessment['confidence'] == 'probable' else '') + assessment['method'],
                '已确认故障注入和设备重新启动；注入条数不代表已确认锁死的核心数，最终复位来源见恢复判断。')
    if evidence['panic']:
        action = ('推测：' if assessment['confidence'] == 'probable' else '') + assessment['method']
        panic_reason = evidence.get('panic_reason', '')
        reason = ('SysRq 请求触发内核崩溃（panic）' if panic_reason == 'sysrq triggered crash'
                  else '内核发生严重错误（panic）' + ('：' + panic_reason if panic_reason else ''))
        return reason, action, '已发现内核崩溃证据；仅确认设备重新启动。'
    if requested:
        detail = ('已观察到请求后的设备启动；原故障是否消除需另行检查。'
                  if history.is_fault_recovery(requested) else '已确认请求后设备重新启动，本次正常重启已完成。')
        return requested['reason'], requested['action'], detail
    if clean:
        return '上一运行周期进入正常关机流程，具体发起原因未确认', '正常关机流程后设备重新启动', '可能为人工重启或关机后重新上电，无法仅凭此记录区分。'
    findings = evidence.get('findings', [])
    if findings:
        symptoms = '、'.join(dict.fromkeys(item['summary'] for item in findings[-3:]))
        timed = any(item.get('uptime') is not None for item in findings)
        return (('异常重启前发现' if timed else '发现缺少时间戳的异常残留：') + symptoms + '，与重启的因果关系待确认',
                '整机重新启动，复位或上电方式未确认',
                '日志确认这些异常曾发生；' + ('可能参与故障，不能仅凭时间接近认定为重启根因。' if timed
                                                else '缺少完整时间线，不能认定这些异常是本次重启根因。'))
    samples = evidence.get('samples', [])
    if samples:
        alerts = [alert for alert in samples[-1].get('alerts', [])
                  if sum(alert in item.get('alerts', []) for item in samples) >= 2]
        reason = '上次运行未留下正常关机标记，观察到非正常中断迹象'
        if alerts:
            reason += '；故障前持续出现' + '、'.join(alerts[:2])
        return (reason, '整机重新启动，复位或上电方式未确认',
                '已关联上次运行采样；候选原因包括掉电、外部复位、卡死或未完成的关机，尚无证据区分。')
    return ('设备重新启动，具体原因未确认', '整机重新启动，复位或上电方式未确认',
            '未取得足够的硬件或崩溃证据，不能区分掉电、人工复位、看门狗复位等情况。')


def observe_boot():
    current_boot = history.boot_id()
    meta = history.metadata()
    if meta.get('boot_id') == current_boot:
        if meta.get('boot_analysis_version') != BOOT_ANALYSIS_VERSION:
            refine_current_boot(current_boot, meta)
        if meta.get('saved_boot_analysis_version') != BOOT_ANALYSIS_VERSION:
            refine_saved_panic_records(current_boot)
        return
    previous = meta.get('boot_id')
    try:
        evidence = boot_evidence(previous, meta.get('pstore_seen', []))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        evidence = {'panic': False, 'software_reboot': False, 'card_reset': False,
                    'evidence': '开机证据读取失败：' + str(exc)[:512], 'seen': meta.get('pstore_seen', []),
                    'errors': [str(exc)[:200]]}
    requested = next((event for event in history.snapshot()['events']
                      if event.get('boot_id') == previous and event.get('kind') == 'board_reboot_request'
                      and event.get('result') in ('pending', 'performed')), None) if previous else None
    reason, action, detail = describe_boot(evidence, requested, meta.get('shutdown_boot_id') == previous and bool(previous))
    confirmed = bool(evidence.get('card_reset') or evidence.get('panic') or requested)
    diagnosis = diagnostics.diagnosis(evidence.get('findings', []), confirmed=confirmed,
                                     limitations=['故障日志确认异常；最终复位方式需硬件证据。'] if confirmed else
                                     evidence.get('board', {}).get('limitations', ['未取得足够复位证据']))
    diagnosis['recovery'] = recovery_assessment(evidence, requested, meta.get('shutdown_boot_id') == previous and bool(previous))
    if evidence.get('hardlockup_count'):
        diagnosis['confidence'] = 'confirmed'
    direct_fault = bool(evidence.get('card_reset') or evidence.get('panic') or evidence.get('hardlockup_count'))
    clean = bool(previous) and meta.get('shutdown_boot_id') == previous
    if direct_fault:
        fault_recovery = True
    elif requested:
        fault_recovery = history.is_fault_recovery(requested)
    else:
        fault_recovery = not clean and bool(previous or evidence.get('findings'))
    category = requested['category'] if requested and not direct_fault else 'system'
    history.record(category, 'system_boot', reason, action, 'performed', detail,
                   event_id=f'boot:{current_boot}', source='开机恢复检查', time_basis='发现时间（故障发生时间未确认）',
                   evidence=evidence['evidence'] or '没有可确认复位方式的证据', diagnosis=diagnosis,
                   fault_recovery=fault_recovery, previous_boot_id=previous,
                   reboot_request_id=requested['id'] if requested else None)
    history.finish_previous_boot(current_boot, previous)
    history.metadata({'boot_id': current_boot, 'previous_boot_id': previous, 'boot_analysis_version': BOOT_ANALYSIS_VERSION,
                      'pstore_seen': evidence['seen'], 'services': {},
                      'boot_evidence_errors': evidence['errors'], 'board_capabilities': evidence.get('board', {})})


def refine_current_boot(current_boot, meta):
    """One-time upgrade of this boot's record from still-fresh crash residues.

    Never use the running kernel's settings to label a prior reset, and never
    relabel older boot records with the latest pstore archive.
    """
    row = next((item for item in history.snapshot()['events']
                if item['id'] == f'boot:{current_boot}' and item.get('kind') == 'system_boot'), None)
    if row:
        evidence = boot_evidence(meta.get('previous_boot_id'), [])
        if evidence.get('panic') or evidence.get('hardlockup_count') or evidence.get('card_reset'):
            reason, action, detail = describe_boot(evidence)
            diagnosis = diagnostics.diagnosis(evidence.get('findings', []), confirmed=True)
            diagnosis['recovery'] = recovery_assessment(evidence)
            history.update(row['id'], row['result'], detail, action=action, reason=reason,
                           evidence=evidence['evidence'] + '\n原记录依据：' + row.get('evidence', ''), diagnosis=diagnosis)
    history.metadata({'boot_analysis_version': BOOT_ANALYSIS_VERSION})


def refine_saved_panic_records(current_boot):
    """Explain old panic records using only each record's own saved evidence."""
    for row in history.snapshot()['events']:
        if (row.get('kind') != 'system_boot' or row.get('boot_id') == current_boot
                or row.get('diagnosis', {}).get('recovery')):
            continue
        text = row.get('evidence', '')
        if 'Kernel panic - not syncing:' not in text:
            continue
        matches = re.findall(r'Kernel panic - not syncing:\s*([^\n]+)',
                             '\n'.join(line for line in text.splitlines() if 'end Kernel panic' not in line))
        evidence = {'panic': True, 'card_reset': False,
                    'panic_reason': matches[-1][:240] if matches else '',
                    'software_reboot': bool(re.search(r'Rebooting in \d+ seconds|reboot: Restarting system', text)),
                    'software_restart': 'reboot: Restarting system' in text,
                    'board': {'limitations': ['历史记录未保存可关联本次重启的硬件复位原因']}}
        diagnosis = dict(row.get('diagnosis') or diagnostics.diagnosis(confirmed=True))
        diagnosis['recovery'] = recovery_assessment(evidence)
        reason, action, _ = describe_boot(evidence)
        history.update(row['id'], row['result'], row['detail'], action=action,
                       reason=reason if matches else row['reason'], diagnosis=diagnosis)
    history.metadata({'saved_boot_analysis_version': BOOT_ANALYSIS_VERSION})


def cause_for_service(unit, status, current_boot):
    result = status.get('Result', '')
    text = ''
    try:
        text = journal(current_boot, unit)
        failures = [line for line in text.splitlines() if re.search(
            r'Failed with result|Main process exited|Watchdog timeout|watchdog.*timed out', line, re.I)]
        text = '\n'.join(failures[-4:])[:2048]
        match = re.findall(r"Failed with result '([^']+)'", text)
        if match:
            result = match[-1]
    except (OSError, RuntimeError, subprocess.SubprocessError):
        pass
    return CAUSES.get(result, '系统检测到服务自动重启，原始触发原因未确认'), text


def observe_services():
    current_boot = history.boot_id()
    listing = command(['systemctl', 'list-units', '--type=service', '--all', '--plain', '--no-legend'])
    units = sorted({part for line in listing.splitlines() for part in line.split()[:2] if UNIT.fullmatch(part)})[:64]
    if not units:
        return
    output = command(['systemctl', 'show', *units,
                      '--property=Id,MainPID,NRestarts,ActiveState,Result,ExecMainCode,ExecMainStatus'])
    states = [dict(line.split('=', 1) for line in block.splitlines() if '=' in line)
              for block in output.strip().split('\n\n')]
    meta = history.metadata()
    previous = meta.get('services', {})
    updated = {}
    for state in states:
        unit = state.get('Id', '')
        if not UNIT.fullmatch(unit):
            continue
        count = int(state.get('NRestarts', '0'))
        old = previous.get(unit, {})
        old_count = old.get('count', 0)
        updated[unit] = {'count': count, 'pid': state.get('MainPID') if state.get('MainPID') not in (None, '0') else old.get('pid', '0'),
                         'count_observed_at': old.get('count_observed_at', 0)}
        category = 'application' if unit.startswith('rk3588-app-') else 'system'
        event_id = f"service:{current_boot}:{unit}:{count}:{state.get('MainPID', '0')}"
        if count > old_count:
            updated[unit]['count_observed_at'] = time.time()
            native = any(event.get('subject') == unit and event.get('kind') == 'service_failure'
                         and event.get('boot_id') == current_boot and event.get('updated_at', 0) > old.get('count_observed_at', 0)
                         for event in history.snapshot()['events'])
            if native:
                if state.get('ActiveState') == 'active':
                    complete_service_start(unit)
                continue
            if old.get('pending_id'):
                history.update(old['pending_id'], 'failed', '服务再次自动重启，上次恢复未稳定运行。')
            reason, evidence = cause_for_service(unit, state, current_boot)
            active = state.get('ActiveState') == 'active' and state.get('MainPID') not in (None, '0')
            history.record(category, 'application_restart' if category == 'application' else 'service_restart',
                           reason, f'systemd 自动重新启动 {unit}', 'performed' if active else 'pending',
                           f"本次观察到自动重启计数从 {old_count} 变为 {count}；"
                           + ('服务进程正在运行，业务状态需另行检查。' if active else '服务尚未恢复运行。'),
                           event_id=event_id, time_basis='发现时间', source='系统服务恢复检查', evidence=evidence)
            updated[unit]['pending_id'] = None if active else event_id
        elif (old.get('pid') not in (None, '0') and state.get('MainPID') not in (None, '0', old.get('pid'))
              and count <= old_count and not old.get('pending_id')):
            history.record(category, 'service_replaced', '观察到服务进程被重新启动，具体发起原因未确认',
                           f"重新启动 {unit}（PID {old['pid']} → {state['MainPID']}）", 'performed',
                           '没有观察到自动重启计数增加，可能为人工操作或其他服务重新启动。',
                           event_id=event_id, time_basis='发现时间', source='系统服务恢复检查')
        elif old.get('pending_id'):
            if state.get('ActiveState') == 'active' and state.get('MainPID') not in (None, '0'):
                history.update(old['pending_id'], 'performed', '服务进程已重新运行，业务状态需另行检查。')
            else:
                updated[unit]['pending_id'] = old['pending_id']
    if updated != previous:
        history.metadata({'services': updated})


def migrate_legacy():
    if history.metadata().get('legacy_imported'):
        return
    from . import manager, network_recovery
    sources = [(manager.EVENTS_FILE, 'application'), (network_recovery.STATE_FILE, 'network'),
               (Path(os.environ.get('KERNEL_RECOVERY_STATE_FILE', '/var/lib/rk3588-recovery/state.json')), 'system')]
    for path, category in sources:
        data = manager.read_json(path, [])
        if isinstance(data, dict):
            data = ([{'time': stamp, 'kind': 'board_reboot_request', 'message': '持续内核阻塞后请求整机重启'}
                     for stamp in data.get('reboots', [])] if category == 'system' else data.get('events', []))
        if not isinstance(data, list):
            continue
        for event in data[-history.KEEP_PER_CATEGORY:]:
            if not isinstance(event, dict) or not isinstance(event.get('time'), (int, float)):
                continue
            if category == 'network' and event.get('kind') not in ('reconnect', 'restart_manager', 'reboot', 'reboot_requested', 'action_error'):
                continue
            message = str(event.get('message', '旧版恢复记录'))
            digest = hashlib.sha256((str(path) + json.dumps(event, sort_keys=True)).encode()).hexdigest()
            history.record(category, str(event.get('kind', 'legacy')), message,
                           '旧版记录未分别保存恢复方式，请结合原始描述判断', 'unknown',
                           '由旧版记录迁移，缺少的信息无法补写。', event_id='legacy:' + digest,
                           timestamp=event['time'], source='历史记录迁移', time_basis='旧版记录时间')
    history.metadata({'legacy_imported': True})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--shutdown', action='store_true')
    parser.add_argument('--scheduled-reboot', action='store_true')
    parser.add_argument('--record-scheduled-reboot', action='store_true')
    parser.add_argument('--scheduled-result', action='store_true')
    parser.add_argument('--once', action='store_true')
    parser.add_argument('--service-exit')
    parser.add_argument('--service-start')
    args = parser.parse_args()
    if args.service_exit:
        record_service_exit(args.service_exit)
        return
    if args.service_start:
        complete_service_start(args.service_start)
        return
    if args.shutdown:
        state = subprocess.run(['systemctl', 'is-system-running'], capture_output=True, text=True, timeout=5)
        if state.stdout.strip() == 'stopping':
            history.record('system', 'system_shutdown', '系统进入正常关机流程，具体发起原因未确认',
                           '系统正在结束服务并准备关机或重启', 'performed',
                           '已观察到关机流程；最终断电或复位方式尚未确认。',
                           event_id='shutdown:' + history.boot_id(), source='系统关机通知', fault_recovery=False)
            history.metadata({'shutdown_boot_id': history.boot_id()})
        return
    if args.scheduled_result:
        event_id = history.metadata().get('scheduled_event_id')
        if event_id:
            failed = os.environ.get('SERVICE_RESULT', 'unknown') != 'success'
            history.update(event_id, 'failed' if failed else 'performed',
                           '定时重启任务执行结果：' + os.environ.get('SERVICE_RESULT', 'unknown'))
        return
    if args.scheduled_reboot or args.record_scheduled_reboot:
        if args.scheduled_reboot:
            from .reboot_coordinator import scheduled_reboot
            scheduled_reboot()
            return
        # Compatibility for old record-only units; the installer migrates their
        # ExecStart to the coordinated CLI before activation.
        event = history.record('system', 'board_reboot_request', '到达每日定时重启时间',
                               'systemd 定时任务请求整机重启', source='每日定时重启', fault_recovery=False)
        history.metadata({'scheduled_event_id': event})
        return
    stopping = threading.Event()
    next_cleanup = 0
    diagnostic_error = None
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, lambda *_: stopping.set())
    while not stopping.is_set():
        try:
            migrate_legacy()
            observe_boot()
            observe_services()
            if time.monotonic() >= next_cleanup:
                try:
                    diagnostics.collect(history.boot_id(), history.metadata().get('services', {}))
                    diagnostic_error = None
                except (OSError, ValueError, subprocess.SubprocessError) as exc:
                    # Diagnostics failure must not prevent recovery event collection.
                    print(f'[FaultDiagnostics] {exc}', flush=True)
                    diagnostic_error = str(exc)[:256]
                from .diagnostic_retention import cleanup
                cleanup()
                next_cleanup = time.monotonic() + 60
            # Health is kept in /run, avoiding a flash write every poll.
            status = {'boot_id': history.boot_id(), 'updated_at': time.time(), 'error': None,
                      'diagnostic_error': diagnostic_error}
        except Exception as exc:
            print(f'[RecoveryHistory] {exc}', flush=True)
            status = {'boot_id': history.boot_id(), 'updated_at': time.time(), 'error': str(exc)[:512]}
        from .manager import atomic_json
        atomic_json(Path('/run/rk3588-recovery-history/status.json'), status)
        notify('WATCHDOG=1')
        if args.once or stopping.wait(15):
            break


if __name__ == '__main__':
    main()
