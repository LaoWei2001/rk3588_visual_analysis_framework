"""Measure an unchanged live application config with an active RTSP consumer.

Does not change clocks, affinity, channel settings, or the source configuration.
Engine and client logs redact RTSP credentials. The copied run config is private.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import tempfile
import threading
import time


def redact(text):
    return re.sub(r'(rtsps?://)[^/\s@]+@', r'\1<redacted>@', text)


def cpu_seconds(pid):
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')


def stop(process, timeout=15):
    if process is None:
        return 0
    before = time.monotonic()
    if process.poll() is None:
        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(5)
            raise RuntimeError('Process required SIGKILL')
    return time.monotonic() - before


def hardware(pid=None):
    result = {}
    paths = [Path('/sys/kernel/debug/rknpu/load'),
             *Path('/sys/class/devfreq').glob('*/cur_freq'),
             *Path('/sys/devices/system/cpu/cpufreq').glob('policy*/scaling_cur_freq'),
             *Path('/sys/class/thermal').glob('thermal_zone*/temp')]
    for path in paths:
        try:
            result[str(path)] = path.read_text().strip()
        except OSError:
            pass
    if pid is not None:
        status = Path(f'/proc/{pid}/status').read_text()
        for name, key in (('VmRSS', 'process_rss_kb'), ('VmHWM', 'process_peak_rss_kb'), ('Threads', 'process_threads')):
            match = re.search(r'^'+name+r':\s*(\d+)', status, re.M)
            if match:
                result[key] = int(match[1])
    return result


def run(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    output.chmod(0o700)
    app = args.app.resolve()
    source = args.config.resolve()
    config_bytes = source.read_bytes()
    config = json.loads(config_bytes)
    config_path = output / 'run-config.json'
    config_path.write_bytes(config_bytes)
    config_path.chmod(0o600)
    global_ = config['global']
    assert global_.get('enable_rtsp'), 'This runner requires RTSP output enabled'
    port, rtsp_path = global_.get('rtsp_port', 8554), global_.get('rtsp_path', '/live')
    expected = {int(c['id']) for c in config['channels'] if c.get('enable', True)}
    with socket.socket() as probe:
        assert probe.connect_ex(('127.0.0.1', port)) != 0, f'Port {port} is already in use'
    events, feed_events, progress = [], [], []
    guard = threading.Lock()
    def drain(process, path, engine):
        with path.open('w') as log:
            for line in process.stdout:
                at = time.monotonic()
                line = redact(line)
                log.write(line)
                if engine and '[Perf][ch' in line:
                    match = re.search(r'\[Perf\]\[ch(\d+)\]', line)
                    values = dict(re.findall(r'([\w+]+)=([\d.]+)', line))
                    with guard:
                        events.append((at, int(match[1]), values))
                if engine and '[Feed][ch' in line:
                    match = re.search(r'\[Feed\]\[ch(\d+)\]', line)
                    values = dict(re.findall(r'([\w+]+)=([\d.]+)', line))
                    with guard:
                        feed_events.append((at, int(match[1]), values))
                if not engine and line.startswith('frame='):
                    with guard:
                        progress.append((at, int(line.split('=', 1)[1])))
                log.flush()
    env = os.environ.copy()
    for key in ('NOTIFY_SOCKET', 'WATCHDOG_PID', 'WATCHDOG_USEC'):
        env.pop(key, None)
    env.update(LD_LIBRARY_PATH=str(app/'libs'), ASSETS_DIR=str(app/'assets'))
    # Linux AF_UNIX paths are limited to 108 bytes; long case names can exceed it.
    sockets = tempfile.TemporaryDirectory(prefix='rk-live-', dir='/tmp')
    socket_dir = Path(sockets.name)
    env.update(RK_LOGIC_CONTROL_SOCKET=str(socket_dir/'logic.sock'),
               RK_DATASET_SOCKET=str(socket_dir/'dataset.sock'),
               RK_FILE_PLAYBACK_CONTROL_SOCKET=str(socket_dir/'playback.sock'))
    binary = args.binary.resolve()
    metadata = dict(binary=str(binary), binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                    started_at=time.strftime('%Y-%m-%dT%H:%M:%S%z'),
                    source_config_sha256=hashlib.sha256(config_bytes).hexdigest(),
                    warmup_seconds=args.warmup, measurement_seconds=args.seconds,
                    channel_count=len(expected), global_config=global_,
                    core_assignments={str(c['id']): [m.get('npu_core', -1) for m in c.get('models', [])]
                                      for c in config['channels']},
                    model_sha256={m['model_path']: hashlib.sha256((app/m['model_path']).read_bytes()).hexdigest()
                                  for c in config['channels'] for m in c.get('models', []) if m.get('enable', True)})
    (output/'metadata.json').write_text(json.dumps(metadata, indent=2))
    process = client = None
    readers = []
    report = {'valid': False, **metadata}
    try:
        process = subprocess.Popen([str(binary), str(config_path)], cwd=app, env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, errors='replace', bufsize=1)
        readers.append(threading.Thread(target=drain, args=(process, output/'engine.log', True)))
        readers[-1].start()
        deadline = time.monotonic()+90
        while time.monotonic() < deadline:
            assert process.poll() is None, 'Engine exited during initialization; inspect engine.log'
            with socket.socket() as probe:
                if probe.connect_ex(('127.0.0.1', port)) == 0:
                    break
            time.sleep(.1)
        else:
            raise RuntimeError('RTSP did not start within 90 seconds')
        client = subprocess.Popen([
            'ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'warning',
            '-progress', 'pipe:1', '-rtsp_transport', 'tcp',
            '-i', f'rtsp://127.0.0.1:{port}{rtsp_path}',
            '-map', '0:v:0', '-an', '-c:v', 'copy', '-f', 'null', '-'],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace', bufsize=1)
        readers.append(threading.Thread(target=drain, args=(client, output/'rtsp.log', False)))
        readers[-1].start()
        time.sleep(args.warmup)
        assert process.poll() is None and client.poll() is None, 'Engine/client stopped during warmup'
        begin = time.monotonic()
        cpu_begin = cpu_seconds(process.pid)
        observations = []
        until = begin+args.seconds
        print(f'{output.name}: exact config running; measuring {args.seconds}s', flush=True)
        last_print = begin
        while time.monotonic() < until:
            time.sleep(min(1, max(0, until-time.monotonic())))
            assert process.poll() is None and client.poll() is None, 'Engine/client stopped'
            sample = {'elapsed': time.monotonic()-begin, **hardware(process.pid)}
            observations.append(sample)
            if time.monotonic()-last_print >= 10:
                with guard:
                    latest = {ch: values.get('infer_fps') for at,ch,values in events if at >= begin}
                print(output.name, round(time.monotonic()-begin), latest, sample.get('/sys/kernel/debug/rknpu/load'), flush=True)
                last_print = time.monotonic()
        end = time.monotonic()
        cpu = 100*(cpu_seconds(process.pid)-cpu_begin)/(end-begin)
        with guard:
            selected = [(at,ch,v) for at,ch,v in events
                        if begin <= at <= end and at-float(v['window']) >= begin]
            active_progress = [(at,frame) for at,frame in progress if begin <= at <= end]
            feeds = [(ch,v) for at,ch,v in feed_events
                     if begin <= at <= end and at-float(v['window']) >= begin]
            assert all(int(v['failed']) == 0 for at,_,v in events if at <= end), 'Inference failed before shutdown'
        assert active_progress and active_progress[-1][1] > active_progress[0][1], 'RTSP output did not advance'
        per_channel = {}
        for channel in sorted(expected):
            windows = [v for _,ch,v in selected if ch == channel]
            assert windows, f'No complete windows for channel {channel}'
            assert all(int(v['failed']) == 0 for v in windows), f'Inference failures on channel {channel}'
            seconds = sum(float(v['window']) for v in windows)
            count = sum(int(v['completed']) for v in windows)
            published = sum(int(v['published']) for v in windows)
            assert count and published, f'No results on channel {channel}'
            metrics = dict(window_seconds=seconds, completed=count, published=published,
                           infer_fps=count/seconds, publish_fps=published/seconds, failures=0)
            for name in ('pre', 'rknn', 'post', 'filter', 'worker', 'wait_q', 'queue+worker'):
                metrics[name+'_ms'] = sum(float(v[name])*int(v['completed']) for v in windows)/count
            channel_feeds = [v for ch,v in feeds if ch == channel]
            if channel_feeds:
                feed_seconds = sum(float(v['window']) for v in channel_feeds)
                metrics.update(decoded_fps=sum(int(v['recv']) for v in channel_feeds)/feed_seconds,
                               enqueued_fps=sum(int(v['enq']) for v in channel_feeds)/feed_seconds,
                               feed_window_seconds=feed_seconds)
            per_channel[str(channel)] = metrics
        loads = [list(map(int,re.findall(r'Core\d+:\s*(\d+)%', s.get('/sys/kernel/debug/rknpu/load',''))))
                 for s in observations]
        loads = [v for v in loads if len(v) == 3]
        assert loads, 'No NPU utilization samples'
        report.update(channels=per_channel, cpu_percent=cpu, samples=observations,
                      npu_load_mean=[sum(v[i] for v in loads)/len(loads) for i in range(3)],
                      total_infer_fps=sum(v['infer_fps'] for v in per_channel.values()),
                      total_publish_fps=sum(v['publish_fps'] for v in per_channel.values()),
                      rtsp_frames_in_window=active_progress[-1][1]-active_progress[0][1])
        report['valid'] = True
    except BaseException as error:
        report['error'] = str(error)
        raise
    finally:
        try:
            stop(client, 8)
        finally:
            try:
                report['shutdown_seconds'] = stop(process, 20)
                if process and process.returncode != 0:
                    report['valid'] = False
                    report['shutdown_returncode'] = process.returncode
            finally:
                for reader in readers:
                    reader.join(5)
                text = (output/'engine.log').read_text() if (output/'engine.log').exists() else ''
                if 'Clean exit (code=0)' not in text:
                    report.update(valid=False, shutdown_error='Missing clean exit')
                if re.search(r'E RKNN:|model inference exception|job abort|FATAL', text):
                    report.update(valid=False, runtime_error='Runtime error logged; inspect engine.log')
                (output/'result.json').write_text(json.dumps(report, indent=2))
                sockets.cleanup()
    assert report['valid'], report.get('runtime_error', report.get('shutdown_error', report))
    print(json.dumps({k:report[k] for k in ('total_infer_fps','total_publish_fps','cpu_percent',
                                          'npu_load_mean','rtsp_frames_in_window','shutdown_seconds')}),
          flush=True)
    return report


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('binary','app','config','output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--warmup', type=float, default=15)
    parser.add_argument('--seconds', type=float, default=60)
    args=parser.parse_args()
    assert args.warmup >= 5 and args.seconds >= 15
    run(args)
