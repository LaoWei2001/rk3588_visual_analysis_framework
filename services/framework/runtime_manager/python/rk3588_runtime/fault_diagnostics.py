"""Bounded, read-only fault evidence; no board registers or watchdog opens."""
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import subprocess
import tempfile
import time

STORE = Path(os.environ.get('RECOVERY_DIAGNOSTICS_FILE', '/var/lib/rk3588-runtime/recovery-diagnostics.json'))
MAX_BYTES = 64 * 1024
PROC = Path('/proc')
SYS = Path('/sys')
WINDOW = 180
# Signatures confirm that a symptom occurred, not that it caused a reset.
PATTERNS = (
    ('hardlockup_test', '人为注入 CPU 硬锁死测试（HARDLOCKUP）', r'lkdtm: Performing direct entry HARDLOCKUP\b'),
    ('panic', '内核崩溃', r'Kernel panic - not syncing'),
    ('oom', '内存不足并触发 OOM', r'Out of memory:|oom-kill:|Killed process \d+ .*total-vm|Memory cgroup out of memory'),
    ('lockup', 'CPU 卡死或调度长期停滞', r'soft lockup|hard LOCKUP|rcu[^\n]*stall|rcu[^\n]*detected stalls'),
    ('blocked', '任务长时间阻塞', r'blocked for more than \d+ seconds|hung_task:|INFO: task .*blocked'),
    ('kernel_fault', '内核访问异常或 Oops', r'Unable to handle kernel|Internal error: Oops|BUG: (?:unable|Bad page|KASAN)|SError Interrupt'),
    ('npu', 'NPU 驱动或任务异常', r'(?:rknpu|rk-npu|RKNPU)[^\n]*(?:timeout|timed out|job[^\n]*failed|failed[^\n]*job|fault|soft reset failed)'),
    ('gpu', 'GPU 驱动或任务异常', r'(?:mali|panthor|panfrost)[^\n]*(?:timeout|timed out|fault|hang|reset[^\n]*fail)'),
    ('codec', '视频编解码驱动异常', r'(?:mpp|rkvdec|rkvenc|vpu)[^\n]*(?:timeout|timed out|fault|reset[^\n]*fail)'),
    ('iommu', 'IOMMU 设备地址访问异常', r'(?:iommu|smmu)[^\n]*(?:page fault|pagefault|unhandled fault|translation fault)'),
    ('storage', '存储或文件系统异常', r'I/O error, dev|Buffer I/O error|EXT4-fs (?:error|warning)|F2FS-fs[^\n]*(?:error|corrupt)|Remounting filesystem read-only|(?:mmc\d|mmcblk\d|nvme\d)[^\n]*(?:timed out|timeout|I/O error|error -\d+)'),
    ('disk_full', '存储空间耗尽', r'No space left on device'),
    ('thermal', '温度达到危险阈值', r'critical temperature|temperature above threshold|thermal[^\n]*(?:critical|shutdown)'),
    ('power', '供电异常告警', r'under[- ]voltage|brown[- ]out|voltage[^\n]*too low'),
    ('network_driver', '网卡驱动或链路异常', r'NETDEV WATCHDOG|(?:r8169|r8125|stmmac|dwmac)[^\n]*(?:timeout|timed out|reset failed)'),
    ('usb', 'USB 控制器或设备异常', r'(?:xhci|usb)[^\n]*(?:host controller not responding|HC died|device descriptor read[^\n]*error|over-current)'),
)
COMPILED = [(code, title, re.compile(pattern, re.I)) for code, title, pattern in PATTERNS]
STAMP = re.compile(r'^\s*(?:<\d+>)?\[\s*(\d+(?:\.\d+)?)\]')


def read(path, limit=4096):
    with Path(path).open('rb') as handle:
        return handle.read(limit).decode(errors='replace').strip().replace('\x00', ',').rstrip(',')


def bounded_command(args):
    # Drain through a capped memory tail; never create an unbounded temp log.
    deadline = time.monotonic() + 5
    tail = bytearray()
    received = 0
    with subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                          env={**os.environ, 'LC_ALL': 'C'}) as process:
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise subprocess.TimeoutExpired(args, 5)
                    if not selector.select(min(remaining, .2)):
                        continue
                    block = os.read(process.stdout.fileno(), 65536)
                    if not block:
                        break
                    received += len(block)
                    if received > 4 * 1024 * 1024:
                        raise RuntimeError('证据输出超过读取上限：' + args[0])
                    tail.extend(block)
                    del tail[:-256 * 1024]
            if process.wait(timeout=max(.01, deadline - time.monotonic())):
                raise RuntimeError('证据命令失败：' + args[0])
        except BaseException:
            process.kill()
            process.wait()
            raise
    return tail.decode(errors='replace')


def signals(text, source, boot, end=None):
    lines = text[-256 * 1024:].splitlines()
    stamps = [float(match[1]) for line in lines if (match := STAMP.match(line))]
    latest = end if end is not None else max(stamps, default=None)
    found = []
    for line in lines:
        stamp = STAMP.match(line)
        uptime = float(stamp[1]) if stamp else None
        # Timestamp-free fragments remain useful, but cannot establish proximity.
        if latest is not None and uptime is not None and not 0 <= latest - uptime <= WINDOW:
            continue
        for code, title, pattern in COMPILED:
            if pattern.search(line):
                excerpt = line[-600:]
                found.append({'code': code, 'summary': title, 'source': source[:160],
                              'line': excerpt, 'uptime': uptime, 'boot_id': boot,
                              'id': hashlib.sha256((boot + source + line).encode()).hexdigest()})
    # Preserve the newest occurrence of each kind and a hard upper bound.
    unique = {item['code']: item for item in found}
    return list(unique.values())[-20:]


def board_capabilities():
    board = {}
    missing = []
    for key, path in [('model', SYS / 'firmware/devicetree/base/model'),
                      ('compatible', SYS / 'firmware/devicetree/base/compatible'),
                      ('kernel', PROC / 'sys/kernel/osrelease')]:
        try:
            board[key] = read(path, 512)
        except OSError:
            missing.append(key)
    board['watchdogs'] = []
    for directory in sorted((SYS / 'class/watchdog').glob('watchdog*'))[:8]:
        info = {'device': directory.name}
        for key in ('identity', 'bootstatus'):
            try:
                info[key] = read(directory / key, 120)
            except OSError:
                pass
        board['watchdogs'].append(info)
    board['reset_sources'] = []
    # Numeric codes deliberately stay raw: their meaning varies by BSP/PMIC.
    for path in [SYS / 'kernel/boot_reason', SYS / 'kernel/reset_reason', SYS / 'kernel/reboot_reason',
                 SYS / 'firmware/devicetree/base/chosen/bootreason',
                 SYS / 'firmware/devicetree/base/chosen/boot-reason']:
        try:
            board['reset_sources'].append({'source': str(path), 'value': read(path, 160)})
        except OSError:
            pass
    try:
        for key, value in re.findall(r'(?:^|\s)((?:androidboot\.)?(?:bootreason|reboot_reason|reset_reason))=([^\s]+)', read(PROC / 'cmdline')):
            board['reset_sources'].append({'source': key, 'value': value[:160]})
    except OSError:
        pass
    board['pstore'] = (SYS / 'fs/pstore').is_dir()
    board['psi'] = {name: (PROC / 'pressure' / name).is_file() for name in ('cpu', 'memory', 'io')}
    if not board['reset_sources']:
        missing.append('板卡未提供可读取的复位原因')
    if not any('bootstatus' in item for item in board['watchdogs']):
        missing.append('看门狗未提供 bootstatus')
    if not all(board['psi'].values()):
        missing.append('部分 PSI 压力接口不可用，使用内存/负载等基础指标')
    board['limitations'] = missing[:6]
    return board


def resources(boot, services=None):
    value = {'boot_id': boot, 'time': time.time(), 'uptime': time.monotonic(), 'alerts': [], 'errors': []}
    try:
        memory = dict(re.findall(r'^(MemTotal|MemAvailable|SwapTotal|SwapFree):\s+(\d+)', read(PROC / 'meminfo'), re.M))
        value['memory_kib'] = {key: int(number) for key, number in memory.items()}
        total, available = int(memory.get('MemTotal', 0)), int(memory.get('MemAvailable', 0))
        if total and 'MemAvailable' in memory and available / total < .03:
            value['alerts'].append('可用内存低于 3%（不能据此确认 OOM）')
    except (OSError, ValueError) as exc:
        value['errors'].append('内存：' + str(exc)[:100])
    try:
        value['load'] = read(PROC / 'loadavg', 160)
    except OSError:
        value['errors'].append('负载不可读')
    value['pressure'] = {}
    for name in ('cpu', 'memory', 'io'):
        try:
            value['pressure'][name] = read(PROC / 'pressure' / name, 256)
        except OSError:
            continue
    value['filesystems'] = []
    for path in dict.fromkeys(['/', '/var/lib', os.environ.get('APPS_ROOT', '/opt/ai_apps')]):
        try:
            stat = os.statvfs(path)
            free = stat.f_bavail * stat.f_frsize
            readonly = bool(stat.f_flag & os.ST_RDONLY)
            value['filesystems'].append({'path': path, 'available_bytes': free, 'readonly': readonly})
            if readonly or (stat.f_blocks and stat.f_bavail / stat.f_blocks < .01):
                value['alerts'].append(f'{path} 文件系统只读' if readonly else f'{path} 剩余空间低于 1%')
        except OSError:
            value['errors'].append(path + ' 空间不可读')
    value['thermal'] = []
    for directory in sorted((SYS / 'class/thermal').glob('thermal_zone*'))[:16]:
        try:
            temp = int(read(directory / 'temp', 32))
            item = {'zone': directory.name, 'type': read(directory / 'type', 80), 'millidegrees': temp}
            # Use the board's critical trip, rather than one temperature for every vendor.
            for kind in sorted(directory.glob('trip_point_*_type'))[:16]:
                if read(kind, 32) == 'critical':
                    critical = int(read(kind.with_name(kind.name.replace('_type', '_temp')), 32))
                    item['critical_millidegrees'] = critical
                    if critical > 0 and temp >= critical:
                        value['alerts'].append(item['type'] + ' 达到设备报告的临界温度')
            value['thermal'].append(item)
        except (OSError, ValueError):
            continue
    value['blocked_tasks'] = []
    for unit, state in list((services or {}).items())[:32]:
        pid = str(state.get('pid', '0'))
        if not pid.isdigit() or pid == '0':
            continue
        for task in sorted((PROC / pid / 'task').glob('*'))[:32]:
            try:
                status = read(task / 'status', 2048)
                if re.search(r'^State:\s+D\b', status, re.M):
                    value['blocked_tasks'].append({'unit': unit[:100], 'tid': task.name,
                                                   'wchan': read(task / 'wchan', 100)})
            except OSError:
                pass
            if len(value['blocked_tasks']) >= 8:
                break
        if len(value['blocked_tasks']) >= 8:
            break
    return value


def recovery_state():
    """Snapshot actual protections before a fault, without opening the watchdog."""
    state = {'errors': []}
    try:
        state['panic_timeout_sec'] = int(read(PROC / 'sys/kernel/panic', 32))
    except (OSError, ValueError) as exc:
        state['errors'].append('panic 设置不可读：' + str(exc)[:100])
    try:
        raw = bounded_command(['systemctl', 'show', '--property=RuntimeWatchdogUSec', '--value']).strip()
        # systemd prints a timespan (e.g. 1min 4s), not an integer number of us.
        from .system_recovery import _seconds
        seconds = 0 if raw in ('0', '0s') else _seconds(raw, None)
        if seconds is None:
            raise ValueError('无法解析实际超时 ' + raw[:80])
        state['watchdog_timeout_sec'] = seconds
        devices = set()
        for entry in (PROC / '1/fd').iterdir():
            try:
                target = os.readlink(entry)
                if target.startswith('/dev/watchdog'):
                    devices.add(target)
            except FileNotFoundError:
                continue
        state['watchdog_manager_devices'] = sorted(devices)[:8]
        state['watchdog_drivers'] = sorted({
            (SYS / 'class/watchdog' / Path(device).name / 'device/driver').resolve().name
            for device in devices
            if (SYS / 'class/watchdog' / Path(device).name / 'device/driver').is_symlink()})[:8]
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        state['errors'].append('硬件看门狗状态读取失败：' + str(exc)[:100])
    return state


@contextmanager
def locked():
    STORE.parent.mkdir(parents=True, exist_ok=True)
    with STORE.with_suffix('.lock').open('a') as handle:
        fcntl.flock(handle, fcntl.LOCK_EX)
        yield


def load():
    try:
        if STORE.stat().st_size > MAX_BYTES:
            raise ValueError('故障采样文件超过上限')
        value = json.loads(STORE.read_text())
        if (not isinstance(value, dict) or value.get('version') != 1
                or not isinstance(value.get('samples'), list) or not isinstance(value.get('signals'), list)
                or len(value['samples']) > 3 or len(value['signals']) > 20
                or not all(isinstance(item, dict) for item in value['samples'] + value['signals'])):
            raise ValueError('故障采样格式错误')
        return value
    except FileNotFoundError:
        return {'version': 1, 'samples': [], 'signals': []}


def collect(boot, services=None):
    with locked():
        old = load()
        if (old['samples'] and old['samples'][-1]['boot_id'] == boot
                and 'recovery_state' in old['samples'][-1]
                and time.monotonic() - old['samples'][-1]['uptime'] < 60):
            return
        sample = resources(boot, services)
        sample['recovery_state'] = recovery_state()
        board = board_capabilities()
        try:
            text = bounded_command(['journalctl', '--no-pager', '-k', '-b', '0', '-o', 'short-monotonic', '-n', '1200', '--since=-3min'])
            recent = signals(text, '当前运行周期内核日志', boot, time.monotonic())
        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
            sample['errors'].append(str(exc)[:160])
            try:
                recent = signals(bounded_command(['dmesg', '--color=never', '--buffer-size=262144']),
                                 '当前内核环形缓冲区', boot, time.monotonic())
            except (OSError, RuntimeError, subprocess.SubprocessError) as fallback:
                recent = []
                sample['errors'].append(str(fallback)[:160])
        ids = {item['id'] for item in old['signals']}
        value = {'version': 1, 'board': board, 'samples': (old['samples'] + [sample])[-3:],
                 'signals': (old['signals'] + [item for item in recent if item['id'] not in ids])[-20:]}
        payload = json.dumps(value, ensure_ascii=False, allow_nan=False).encode()
        while len(payload) > MAX_BYTES and value['signals']:
            value['signals'].pop(0)
            payload = json.dumps(value, ensure_ascii=False, allow_nan=False).encode()
        if len(payload) > MAX_BYTES:
            raise ValueError('故障采样内容超过上限')
        # Only one writer under this lock; remove crash-left temporary files.
        for orphan in STORE.parent.glob('.recovery-diagnostics-*'):
            if orphan.is_file() and not orphan.is_symlink():
                orphan.unlink(missing_ok=True)
        fd, name = tempfile.mkstemp(prefix='.recovery-diagnostics-', dir=STORE.parent)
        try:
            with os.fdopen(fd, 'wb') as handle:
                handle.write(payload)
                handle.flush()
                os.fsync(handle.fileno())
            os.replace(name, STORE)
            directory = os.open(STORE.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
        finally:
            Path(name).unlink(missing_ok=True)


def previous_context(previous_boot):
    value = load()
    samples = [item for item in value['samples'] if item.get('boot_id') == previous_boot] if previous_boot else []
    last = samples[-1] if samples else None
    found = [item for item in value['signals'] if item.get('boot_id') == previous_boot
             and last and item.get('uptime') is not None and 0 <= last['uptime'] - item['uptime'] <= WINDOW]
    return samples, found


def diagnosis(found=(), *, confirmed=False, limitations=()):
    return {'confidence': 'confirmed' if confirmed else ('probable' if found else 'unknown'),
            'findings': [{'code': item['code'], 'summary': item['summary']} for item in list(found)[-3:]],
            'limitations': list(limitations)[:2]}


def status():
    """Small UI view; raw state and process details stay in bounded evidence."""
    try:
        value = load()
        last = value['samples'][-1] if value['samples'] else {}
        return {'board': value.get('board'), 'sample_time': last.get('time'),
                'errors': last.get('errors', [])[:4], 'error': None}
    except (OSError, ValueError, TypeError) as exc:
        return {'board': None, 'error': str(exc)[:160]}
