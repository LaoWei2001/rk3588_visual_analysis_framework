"""Small, durable recovery history shared by all independent services.

The entire file is replaced atomically. Both entry count and individual fields
are bounded, so rotation cannot leave an ever-growing archive or database.
"""
from contextlib import contextmanager
import fcntl
import json
import math
import os
from pathlib import Path
import tempfile
import threading
import time
import uuid

HISTORY_FILE = Path(os.environ.get('RECOVERY_HISTORY_FILE', '/var/lib/rk3588-runtime/recovery-history.json'))
BOOT_ID_FILE = Path('/proc/sys/kernel/random/boot_id')
KEEP_PER_CATEGORY = 20
KEEP_OPERATIONS = 20
MAX_FILE_BYTES = 512 * 1024
CATEGORIES = ('system', 'network', 'application')
_lock = threading.RLock()


def is_fault_recovery(event):
    """Separate recovery incidents from routine operation audit, including v1 history."""
    if isinstance(event.get('fault_recovery'), bool):
        return event['fault_recovery']
    category, kind = event.get('category'), event.get('kind')
    if category == 'system':
        if kind not in ('system_boot', 'board_reboot_request'):
            return False
        reason = event.get('reason', '')
        return not (event.get('source') == '每日定时重启'
                    or reason.startswith(('到达每日定时重启时间', '上一运行周期进入正常关机流程')))
    if category == 'network':
        return kind in ('reconnect', 'restart_manager', 'reboot', 'reboot_requested', 'board_reboot_request',
                        'action_error', 'external_recovery', 'network_rollback')
    if category == 'application':
        return kind in ('service_failure', 'application_restart', 'restore_error', 'service_error')
    return False


def _clip(value, limit):
    clipped = str(value).encode('utf-8', errors='replace')[:limit].decode('utf-8', errors='ignore')
    while len(json.dumps(clipped, ensure_ascii=False).encode()) > limit:
        clipped = clipped[:len(clipped) // 2]
    return clipped


def _diagnosis(value):
    result = {'confidence': value.get('confidence') if value.get('confidence') in ('confirmed', 'probable', 'unknown') else 'unknown',
              'findings': [{'code': _clip(item.get('code', ''), 40), 'summary': _clip(item.get('summary', ''), 160)}
                           for item in value.get('findings', [])[-3:] if isinstance(item, dict)],
              'limitations': [_clip(item, 180) for item in value.get('limitations', [])[:2]]}
    recovery = value.get('recovery')
    if isinstance(recovery, dict):
        result['recovery'] = {
            'confidence': recovery.get('confidence') if recovery.get('confidence') in ('confirmed', 'probable', 'unknown') else 'unknown',
            'method': _clip(recovery.get('method', ''), 180),
            'basis': [_clip(item, 240) for item in recovery.get('basis', [])[:3]],
            'limitations': [_clip(item, 240) for item in recovery.get('limitations', [])[:3]],
        }
    while len(json.dumps(result, ensure_ascii=False).encode()) > 1024:
        if result['findings']:
            result['findings'].pop(0)
        elif result['limitations']:
            result['limitations'].pop()
        elif result.get('recovery', {}).get('basis'):
            result['recovery']['basis'].pop()
        elif result.get('recovery', {}).get('limitations'):
            result['recovery']['limitations'].pop()
        else:
            break
    return result


def boot_id():
    return BOOT_ID_FILE.read_text().strip()


def _read():
    try:
        if HISTORY_FILE.stat().st_size > MAX_FILE_BYTES:
            raise ValueError('恢复记录文件超过大小限制，请检查文件')
        value = json.loads(HISTORY_FILE.read_text())
    except FileNotFoundError:
        return {'version': 1, 'events': [], 'meta': {}}
    if (not isinstance(value, dict) or value.get('version') != 1
            or not isinstance(value.get('events'), list) or not isinstance(value.get('meta'), dict)):
        raise ValueError('恢复记录文件格式异常')
    return value


@contextmanager
def locked():
    with _lock:
        HISTORY_FILE.parent.mkdir(parents=True, exist_ok=True)
        with HISTORY_FILE.with_suffix('.lock').open('a+') as handle:
            fcntl.flock(handle, fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(handle, fcntl.LOCK_UN)


def _write(value):
    # Faults and operation audit have independent count limits, sharing the same
    # hard byte limit. Operations are evicted first if the byte budget is tight.
    selected = set()
    for category in CATEGORIES:
        indices = [index for index in range(len(value['events']) - 1, -1, -1)
                   if value['events'][index].get('category') == category]
        faults = [index for index in indices if is_fault_recovery(value['events'][index])][:KEEP_PER_CATEGORY]
        selected.update(faults)
    operations = [index for index in range(len(value['events']) - 1, -1, -1)
                  if value['events'][index].get('category') in CATEGORIES
                  and not is_fault_recovery(value['events'][index])][:KEEP_OPERATIONS]
    selected.update(operations)
    value['events'] = [event for index, event in enumerate(value['events']) if index in selected]
    payload = (json.dumps(value, ensure_ascii=False, allow_nan=False) + '\n').encode()
    while len(payload) > MAX_FILE_BYTES:
        oldest = next((index for index, event in enumerate(value['events']) if not is_fault_recovery(event)), None)
        if oldest is None:
            break
        value['events'].pop(oldest)
        payload = (json.dumps(value, ensure_ascii=False, allow_nan=False) + '\n').encode()
    if len(payload) > MAX_FILE_BYTES:
        raise ValueError('恢复记录内容超过大小限制')
    fd, filename = tempfile.mkstemp(prefix='.recovery-history-', dir=HISTORY_FILE.parent)
    temporary = Path(filename)
    try:
        with os.fdopen(fd, 'wb') as output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
        temporary.replace(HISTORY_FILE)
        directory = os.open(HISTORY_FILE.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        temporary.unlink(missing_ok=True)


def record(category, kind, reason, action, result='pending', detail='', *,
           event_id=None, timestamp=None, source='恢复服务', evidence='', time_basis='触发时间',
           source_boot_id=None, subject='', diagnosis=None, fault_recovery=None,
           previous_boot_id=None, reboot_request_id=None):
    if category not in CATEGORIES or result not in ('pending', 'performed', 'recovered', 'failed', 'unknown'):
        raise ValueError('恢复记录类别或结果无效')
    timestamp = time.time() if timestamp is None else timestamp
    if not isinstance(timestamp, (int, float)) or not math.isfinite(timestamp):
        raise ValueError('恢复记录时间无效')
    event = {'id': _clip(event_id or uuid.uuid4(), 160), 'time': timestamp, 'updated_at': time.time(),
             'category': category, 'kind': _clip(kind, 80), 'reason': _clip(reason, 512),
             'action': _clip(action, 512), 'result': result, 'detail': _clip(detail, 1024),
             'source': _clip(source, 120), 'evidence': _clip(evidence, 2048),
             'subject': _clip(subject, 160),
             'time_basis': _clip(time_basis, 80), 'boot_id': _clip(source_boot_id or boot_id(), 80)}
    event['message'] = _clip(f"{event['reason']}；{event['action']}", 768)
    if previous_boot_id:
        event['previous_boot_id'] = _clip(previous_boot_id, 80)
    if reboot_request_id:
        event['reboot_request_id'] = _clip(reboot_request_id, 160)
    if diagnosis is not None:
        event['diagnosis'] = _diagnosis(diagnosis)
    if fault_recovery is not None:
        if not isinstance(fault_recovery, bool):
            raise ValueError('故障恢复标记无效')
        event['fault_recovery'] = fault_recovery
    with locked():
        value = _read()
        if any(item.get('id') == event['id'] for item in value['events']):
            return event['id']
        value['events'].append(event)
        _write(value)
    return event['id']


def update(event_id, result, detail, evidence=None, action=None, diagnosis=None, reason=None):
    if result not in ('pending', 'performed', 'recovered', 'failed', 'unknown'):
        raise ValueError('恢复结果无效')
    with locked():
        value = _read()
        for event in value['events']:
            if event.get('id') == event_id:
                event.update(result=result, detail=_clip(detail, 1024), updated_at=time.time())
                if evidence is not None:
                    event['evidence'] = _clip(evidence, 2048)
                if action is not None:
                    event['action'] = _clip(action, 512)
                if reason is not None:
                    event['reason'] = _clip(reason, 512)
                if diagnosis is not None:
                    event['diagnosis'] = _diagnosis(diagnosis)
                event['message'] = _clip(f"{event['reason']}；{event['action']}", 768)
                _write(value)
                return True
    return False  # An old record may already have rotated out.


def metadata(changes=None):
    with locked():
        value = _read()
        if changes:
            value['meta'].update(changes)
            _write(value)
        return dict(value['meta'])


def _normal_boot_events(value):
    """Present one normal reboot with its stages; retain the raw audit on disk.

    Boot IDs and request IDs establish the relationship. Wall-clock proximity
    cannot do so, especially after manual clock changes. Fault recovery attempts
    and independent startup operations must remain separate records.
    """
    events = value['events']
    by_id = {event.get('id'): event for event in events}
    merged, consumed = {}, set()
    for boot in reversed(events):
        if boot.get('kind') != 'system_boot' or is_fault_recovery(boot):
            continue
        current = boot.get('boot_id')
        previous = boot.get('previous_boot_id')
        if not previous and current == value['meta'].get('boot_id'):
            previous = value['meta'].get('previous_boot_id')
        request = by_id.get(boot.get('reboot_request_id'))
        if not request:
            # Compatibility with records saved before boot links were explicit.
            request = next((event for event in reversed(events)
                            if event.get('kind') == 'board_reboot_request'
                            and event.get('recovered_boot_id') == current
                            and event.get('reason') == boot.get('reason')
                            and event.get('action') == boot.get('action')), None)
        if request and (request.get('kind') != 'board_reboot_request'
                        or request.get('category') != boot.get('category')
                        or request.get('result') not in ('pending', 'performed')
                        or is_fault_recovery(request) or request.get('id') in consumed
                        or not request.get('boot_id') or request.get('boot_id') == current
                        or (previous and request.get('boot_id') != previous)):
            request = None
        if request:
            previous = request['boot_id']
        shutdown = by_id.get('shutdown:' + previous) if previous else None
        if shutdown and (shutdown.get('kind') != 'system_shutdown'
                         or shutdown.get('boot_id') != previous
                         or shutdown.get('id') in consumed or is_fault_recovery(shutdown)):
            shutdown = None
        stages = [event for event in (request, shutdown, boot) if event]
        if len(stages) < 2:
            continue
        row = dict(boot)
        row['stages'] = [{key: stage.get(key, '') for key in
                          ('id', 'kind', 'time', 'time_basis', 'action', 'result', 'detail', 'source')}
                         for stage in stages]
        row['updated_at'] = max(stage.get('updated_at', stage['time']) for stage in stages)
        if request:
            row.update(time=request['time'], time_basis=request.get('time_basis', '触发时间'),
                       source=request['source'], detail='已确认请求后设备重新启动，本次正常重启已完成。')
        merged[boot['id']] = row
        consumed.update(stage['id'] for stage in stages[:-1])
    return [merged.get(event['id'], event) for event in events if event['id'] not in consumed]


def snapshot(category=None):
    # Atomic replacement makes reads safe without taking a writable lock.
    try:
        value = _read()
        events = [{**event, 'fault_recovery': is_fault_recovery(event)} for event in reversed(_normal_boot_events(value))
                  if category is None or event.get('category') == category]
        from . import fault_diagnostics
        return {'events': events, 'keep_per_category': KEEP_PER_CATEGORY, 'keep_operations': KEEP_OPERATIONS, 'error': None,
                'observer': value['meta'].get('observer', {}), 'diagnostics': fault_diagnostics.status()}
    except (OSError, ValueError) as exc:
        return {'events': [], 'keep_per_category': KEEP_PER_CATEGORY, 'keep_operations': KEEP_OPERATIONS,
                'error': str(exc), 'observer': {}}


def request_reboot(reason, action, *, category='system', detail='', source='恢复服务'):
    return record(category, 'board_reboot_request', reason, action, detail=detail, source=source)


def finish_previous_boot(current_boot, previous_boot):
    """A new boot proves startup, not that the original network/business fault is fixed."""
    with locked():
        value = _read()
        changed = False
        for event in value['events']:
            if (previous_boot and event.get('kind') == 'board_reboot_request'
                    and event.get('boot_id') == previous_boot and previous_boot != current_boot
                    and not event.get('recovered_boot_id')
                    and event.get('result') in ('pending', 'performed')):
                detail = ('请求后已观察到设备重新启动；原故障是否消除需另行检查。'
                          if is_fault_recovery(event) else '已确认请求后设备重新启动，本次正常重启已完成。')
                event.update(result='performed', updated_at=time.time(),
                             detail=detail,
                             recovered_boot_id=current_boot)
                changed = True
        if changed:
            _write(value)


def safe_record(*args, **kwargs):
    """Application detection must keep working even if history storage fails."""
    try:
        return record(*args, **kwargs)
    except (OSError, ValueError) as exc:
        print(f'[RecoveryHistory] 记录保存失败：{exc}', flush=True)
        return None


def safe_update(*args, **kwargs):
    try:
        return update(*args, **kwargs)
    except (OSError, ValueError) as exc:
        print(f'[RecoveryHistory] 记录更新失败：{exc}', flush=True)
        return False
