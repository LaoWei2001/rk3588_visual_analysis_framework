"""Alternate two engine binaries against identical video/model workloads.

Requires an idle NPU. Does not change service configuration, clocks or affinity.
Keep RTSP decoding active and measure successful inference AND published results.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import time


def counters(pid):
    # utime + stime; one fully occupied CPU core is reported as 100%.
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')


def perf_lines(text):
    rows = {}
    for line in text.splitlines():
        match = re.search(r'\[Perf\]\[ch(\d+)\]', line)
        if not match:
            continue
        values = dict(re.findall(r'([\w+]+)=([\d.]+)', line))
        assert int(values['failed']) == 0, line
        rows.setdefault(match[1], []).append(values)
    return rows


def summarize(text):
    result = {}
    for channel, windows in perf_lines(text).items():
        seconds = sum(float(w['window']) for w in windows)
        completed = sum(int(w['completed']) for w in windows)
        published = sum(int(w['published']) for w in windows)
        assert completed > 0 and published > 0
        result[channel] = dict(seconds=seconds, completed=completed, published=published,
            infer_fps=completed / seconds, publish_fps=published / seconds,
            queue_worker_ms=sum(float(w['queue+worker']) * int(w['completed']) for w in windows) / completed)
    return result


def stop(proc, timeout):
    if proc and proc.poll() is None:
        proc.send_signal(signal.SIGINT)
        try:
            proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
            raise RuntimeError('Process did not stop normally')


def comparison(report):
    summary = {}
    for run in report['runs']:
        group = summary.setdefault(run['case'], {}).setdefault(run['variant'], {})
        for channel, values in run['channels'].items():
            counts = group.setdefault(channel, dict(seconds=0, completed=0, published=0, latency_sum=0))
            for key in ('seconds', 'completed', 'published'): counts[key] += values[key]
            counts['latency_sum'] += values['queue_worker_ms'] * values['completed']
    for variants in summary.values():
        for channels in variants.values():
            for counts in channels.values():
                counts['infer_fps'] = counts['completed'] / counts['seconds']
                counts['publish_fps'] = counts['published'] / counts['seconds']
                counts['queue_worker_ms'] = counts.pop('latency_sum') / counts['completed']
        if 'baseline' in variants and 'candidate' in variants:
            variants['change_percent'] = {
                channel: {metric: 100 * (values[metric] / variants['baseline'][channel][metric] - 1)
                          for metric in ('infer_fps', 'publish_fps', 'queue_worker_ms')}
                for channel, values in variants['candidate'].items()}
    return summary


def regressions(summary, allowed_percent=0.0):
    """Inspect every channel, including published results and latency."""
    failures = []
    for case, variants in summary.items():
        for channel, metrics in variants.get('change_percent', {}).items():
            for metric, change in metrics.items():
                loss = change if metric == 'queue_worker_ms' else -change
                if loss > allowed_percent:
                    failures.append(dict(case=case, channel=channel, metric=metric,
                                         change_percent=change))
    return failures


def run(binary, app, config_path, directory, warmup, seconds):
    env = os.environ.copy()
    env.update(LD_LIBRARY_PATH=str(app / 'libs'), ASSETS_DIR=str(app / 'assets'))
    logpath, rtsp = directory / 'engine.log', directory / 'rtsp.log'
    config = json.loads(config_path.read_text())
    with logpath.open('w') as log, rtsp.open('w') as video:
        proc = subprocess.Popen([str(binary), str(config_path)], cwd=app, env=env,
                                stdout=log, stderr=subprocess.STDOUT)
        client = None
        try:
            for _ in range(600):
                assert proc.poll() is None, 'Engine exited during startup'
                try:
                    with socket.create_connection(('127.0.0.1', 18554), timeout=.1):
                        break
                except OSError:
                    time.sleep(.1)
            else:
                raise RuntimeError('RTSP listener not ready')
            client = subprocess.Popen(['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error',
                '-progress', 'pipe:1', '-rtsp_transport', 'tcp', '-i', 'rtsp://127.0.0.1:18554/live',
                '-an', '-vsync', '0', '-enc_time_base', '-1', '-f', 'null', '-'],
                stdout=video, stderr=subprocess.STDOUT)
            time.sleep(warmup)
            # Discard the window crossing the warmup boundary for each channel.
            offset = logpath.stat().st_size
            cpu_start, started = counters(proc.pid), time.monotonic()
            frames = -1
            observations = []
            deadline = started + seconds
            while time.monotonic() < deadline:
                time.sleep(min(10, max(0, deadline - time.monotonic())))
                assert proc.poll() is None and client.poll() is None, 'Engine or RTSP client stopped'
                full = logpath.read_text(errors='replace')
                assert not re.search(r'model inference exception|E RKNN:|FATAL|Model load error|failed to submit', full)
                rows = perf_lines(full)
                assert len(rows) == len(config['channels']), 'Missing channel metrics'
                assert all(int(w[-1]['completed']) > 0 for w in rows.values()), 'Inference stopped'
                seen = re.findall(r'^frame=(\d+)', rtsp.read_text(errors='replace'), re.M)
                assert seen and int(seen[-1]) > frames, 'RTSP stopped advancing'
                frames = int(seen[-1])
                hardware = {}
                for path in [Path('/sys/kernel/debug/rknpu/load'),
                             *Path('/sys/class/devfreq').glob('*npu/cur_freq'),
                             *Path('/sys/class/thermal').glob('thermal_zone*/temp')]:
                    try: hardware[str(path)] = path.read_text().strip()
                    except OSError: pass
                observations.append(hardware)
                print(directory.name, round(time.monotonic()-started), 's',
                      {ch: w[-1]['infer_fps'] for ch, w in rows.items()}, flush=True)
            cpu_percent = 100 * (counters(proc.pid) - cpu_start) / (time.monotonic() - started)
            text = logpath.read_bytes()[offset:].decode(errors='replace')
            seen_channels, complete = set(), []
            for line in text.splitlines():
                match = re.search(r'\[Perf\]\[ch(\d+)\]', line)
                if match:
                    if match[1] in seen_channels: complete.append(line)
                    seen_channels.add(match[1])
            summary = summarize('\n'.join(complete))
            assert len(summary) == len(config['channels'])
            result = dict(channels=summary, cpu_percent=cpu_percent, decoded_frames=frames,
                          hardware=observations)
        finally:
            # Always release the NPU even when the client/measurement fails.
            try: stop(client, 5)
            finally:
                start = time.monotonic()
                stop(proc, 10)
                shutdown = time.monotonic() - start
        assert proc.returncode == 0 and 'Clean exit (code=0)' in logpath.read_text(errors='replace')
        result['shutdown_seconds'] = shutdown
        (directory / 'result.json').write_text(json.dumps(result, indent=2))
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('baseline', 'candidate', 'app', 'config', 'manifest', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=60)
    parser.add_argument('--warmup', type=int, default=15)
    parser.add_argument('--repeats', type=int, default=2)
    parser.add_argument('--max-regression-percent', type=float, default=0.0,
                        help='Allowed per-channel regression; default 0 (strict no-loss check)')
    parser.add_argument('--cases', nargs='+', choices=['dual', 'channels', 'composite'],
                        default=['dual', 'composite'])
    args = parser.parse_args()
    assert args.seconds >= 30 and args.warmup >= 10 and args.repeats >= 1
    assert args.max_regression_percent >= 0
    app, output = args.app.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    binaries = dict(baseline=args.baseline.resolve(), candidate=args.candidate.resolve())
    report = dict(binaries={k: dict(path=str(v), sha256=hashlib.sha256(v.read_bytes()).hexdigest())
                           for k, v in binaries.items()}, runs=[])
    base = json.loads(args.config.read_text())
    models = json.loads(args.manifest.read_text())
    for case in args.cases:
        config = copy.deepcopy(base)
        config['global']['rtsp_port'] = 18554
        if case != 'dual':
            template = config['channels'][0]
            groups = [[m] for m in models] if case == 'channels' else [models]
            config['channels'] = []
            for index, group in enumerate(groups):
                channel = copy.deepcopy(template)
                channel.update(id=index, models=copy.deepcopy(group))
                config['channels'].append(channel)
            config['global'].update(tile_rows=2 if case == 'channels' else 1,
                                    tile_cols=3 if case == 'channels' else 1)
        for repeat in range(args.repeats):
            order = ('baseline', 'candidate') if repeat % 2 == 0 else ('candidate', 'baseline')
            for variant in order:
                directory = output / f'{case}-{repeat}-{variant}'
                directory.mkdir()
                path = directory / 'config.json'
                path.write_text(json.dumps(config, indent=2))
                result = run(binaries[variant], app, path, directory, args.warmup, args.seconds)
                report['runs'].append(dict(case=case, repeat=repeat, variant=variant, **result))
                (output / 'report.json').write_text(json.dumps(report, indent=2))
    summary = comparison(report)
    (output / 'summary.json').write_text(json.dumps(summary, indent=2))
    failures = regressions(summary, args.max_regression_percent)
    (output / 'acceptance.json').write_text(json.dumps(dict(
        accepted=not failures, max_regression_percent=args.max_regression_percent,
        regressions=failures), indent=2))
    print('COMPARISON COMPLETE', output / 'report.json', flush=True)
    if failures:
        print('PERFORMANCE CHECK FAILED: inspect per-channel regressions in acceptance.json', flush=True)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
