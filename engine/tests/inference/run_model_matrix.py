"""Board regression for multiple video streams, mixed cores and model reload.

Requires an idle NPU. Configurations/logs are written only to --output; the app's
saved configuration is never changed. Manifest is a JSON array of model nodes.
"""
import argparse
import copy
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--app', type=Path, required=True)
    p.add_argument('--base-config', type=Path, required=True)
    p.add_argument('--manifest', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--case', choices=['channels_auto', 'channels_fixed', 'composite_auto',
                                     'composite_fixed', 'reload_mixed', 'serial_fallback'])
    p.add_argument('--thread-fault-library', type=Path)
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    base = json.loads(args.base_config.read_text())
    models = json.loads(args.manifest.read_text())
    report = []
    cases = [('channels_auto', 120), ('channels_fixed', 60),
             ('composite_auto', 120), ('composite_fixed', 60), ('reload_mixed', 120)]
    if args.case == 'serial_fallback':
        assert args.thread_fault_library, 'serial fallback requires a thread fault injection library'
        cases = [('serial_fallback', 60)]
    elif args.case: cases = [case for case in cases if case[0] == args.case]
    for name, duration in cases:
        config = copy.deepcopy(base)
        config['global'].update(channel_threads=3, rtsp_port=18554, enable_display=False,
                                enable_rtsp=True, performance_display=True, tile_rows=2, tile_cols=3)
        template = config['channels'][0]
        groups = [[model] for model in models] if name.startswith('channels') else [models]
        config['channels'] = []
        for index, group in enumerate(groups):
            channel = copy.deepcopy(template)
            channel.update(id=index, models=copy.deepcopy(group), logic='logic_default')
            for model in channel['models']:
                model['npu_core'] = 0 if name.endswith('fixed') else -1
            config['channels'].append(channel)
        if len(groups) == 1: config['global'].update(tile_rows=1, tile_cols=1)
        path = args.output / (name + '.json')
        path.write_text(json.dumps(config, ensure_ascii=False, indent=2))
        logpath = args.output / (name + '.log')
        rtsp = args.output / (name + '-rtsp.log')
        env = os.environ.copy()
        env.update(LD_LIBRARY_PATH=str(args.app / 'libs'), ASSETS_DIR=str(args.app / 'assets'))
        if args.thread_fault_library: env['LD_PRELOAD'] = str(args.thread_fault_library.resolve())
        start = time.monotonic()
        frames = -1
        with logpath.open('w') as log, rtsp.open('w') as video:
            proc = subprocess.Popen([str(args.binary.resolve()), str(path.resolve())], cwd=args.app,
                                    env=env, stdout=log, stderr=subprocess.STDOUT)
            client = None
            try:
                for _ in range(300):
                    if proc.poll() is not None: raise RuntimeError(f'{name}: startup exit {proc.returncode}')
                    try:
                        with socket.create_connection(('127.0.0.1', 18554), timeout=.1): pass
                        break
                    except OSError: time.sleep(.1)
                else: raise RuntimeError('RTSP listener not ready')
                client = subprocess.Popen(['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error',
                    '-progress', 'pipe:1', '-rtsp_transport', 'tcp', '-i', 'rtsp://127.0.0.1:18554/live',
                    '-an', '-vsync', '0', '-enc_time_base', '-1', '-f', 'null', '-'],
                    stdout=video, stderr=subprocess.STDOUT)
                for elapsed in range(15, duration + 1, 15):
                    try: proc.wait(timeout=15); raise RuntimeError('engine exited early')
                    except subprocess.TimeoutExpired: pass
                    text = logpath.read_text(errors='replace')
                    errors = [line for line in text.splitlines() if re.search(
                        r'model inference exception|E RKNN:|FATAL|Model load error|failed to submit|rknn_.*failed', line)]
                    assert not errors, errors[-5:]
                    if name == 'serial_fallback':
                        assert 'cannot start parallel model workers' in text, 'fault did not reach composite executor'
                    assert client.poll() is None, 'RTSP client exited'
                    seen = re.findall(r'^frame=(\d+)', rtsp.read_text(errors='replace'), re.M)
                    assert seen and int(seen[-1]) > frames, 'RTSP decode stalled'
                    frames = int(seen[-1])
                    summary = []
                    for ch in config['channels']:
                        prefix = f"[Perf][ch{ch['id']:02d}]"
                        windows = [line for line in text.splitlines() if prefix in line]
                        assert windows and all('failed=0' in line for line in windows), windows[-3:]
                        assert int(re.search(r'completed=(\d+)', windows[-1])[1]) > 0
                        summary.append(re.search(r'infer_fps=([\d.]+)', windows[-1])[1])
                    print(name, elapsed, 'seconds; infer_fps', ','.join(summary), 'decoded', frames, flush=True)
                    if name == 'reload_mixed' and elapsed in (30, 75):
                        config['channels'][0]['models'].reverse()
                        for i, model in enumerate(config['channels'][0]['models']):
                            model['npu_core'] = [0, -1, 2][(i + elapsed // 15) % 3]
                        temporary = path.with_suffix('.new')
                        temporary.write_text(json.dumps(config, ensure_ascii=False, indent=2))
                        temporary.replace(path)
                    if name == 'reload_mixed' and elapsed >= 60:
                        expected = 2 if elapsed >= 105 else 1
                        assert text.count('model reload complete') >= expected, 'hot reload did not complete'
            finally:
                if client and client.poll() is None:
                    client.send_signal(signal.SIGINT)
                    try: client.wait(timeout=5)
                    except subprocess.TimeoutExpired: client.kill(); client.wait()
                stopping = time.monotonic()
                if proc.poll() is None: proc.send_signal(signal.SIGINT)
                try: proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    try: proc.wait(timeout=5)
                    except subprocess.TimeoutExpired: pass
                shutdown = time.monotonic() - stopping
                print(name, 'exit', proc.returncode, 'shutdown', round(shutdown, 3), flush=True)
            assert proc.returncode == 0 and 'Clean exit (code=0)' in logpath.read_text(errors='replace')
        report.append(dict(case=name, seconds=round(time.monotonic() - start, 2), decoded_frames=frames,
                           shutdown_seconds=round(shutdown, 3), result='PASS'))
        (args.output / 'report.json').write_text(json.dumps(report, indent=2))
    print('ALL MODEL MATRIX CASES PASSED', flush=True)


if __name__ == '__main__': main()
