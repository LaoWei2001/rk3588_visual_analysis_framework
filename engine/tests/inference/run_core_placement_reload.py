"""RK3588 regression: global AUTO placement follows hot reload and rollback.

Requires three active single-model channels, an idle NPU and an isolated app.
Only a private temporary configuration is changed; camera URLs stay in private logs.
"""
import argparse
import copy
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import time

from run_npu_performance import perf_lines, stop


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'app', 'config', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    root, app = args.output.resolve(), args.app.resolve()
    root.mkdir(mode=0o700, parents=True, exist_ok=False)
    base = json.loads(args.config.read_text())
    assert len(base['channels']) == 3
    for index, channel in enumerate(base['channels']):
        assert channel['id'] == index and channel['enable']
        assert len(channel['models']) == 1
        channel['models'][0]['npu_core'] = index
    base['global']['rtsp_port'] = 18554
    path, logpath, rtsp = root / 'config.private.json', root / 'engine.log', root / 'rtsp.log'

    def save(config):
        temporary = path.with_suffix('.new')
        temporary.write_text(json.dumps(config, indent=2))
        temporary.chmod(0o600)
        temporary.replace(path)

    save(base)
    env = os.environ.copy()
    env.update(LD_LIBRARY_PATH=str(app / 'libs'), ASSETS_DIR=str(app / 'assets'),
               EVENT_STORE_DIR=str(root / 'events'), RK_LOGIC_CONTROL_SOCKET=str(root / 'control.sock'),
               RK_FILE_PLAYBACK_CONTROL_SOCKET=str(root / 'playback.sock'), RK_DATASET_SOCKET=str(root / 'dataset.sock'))
    for key in ('NOTIFY_SOCKET', 'WATCHDOG_PID', 'WATCHDOG_USEC'):
        env.pop(key, None)
    report, client = [], None
    with logpath.open('w') as log, rtsp.open('w') as video:
        proc = subprocess.Popen([str(args.binary.resolve()), str(path)], cwd=app, env=env,
                                stdout=log, stderr=subprocess.STDOUT)

        def wait_for(check, timeout=60):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                assert proc.poll() is None, 'engine stopped'
                if check():
                    return
                time.sleep(.2)
            raise AssertionError('timed out waiting for reload/progress')

        def frames():
            seen = re.findall(r'^frame=(\d+)', rtsp.read_text(errors='replace'), re.M)
            return int(seen[-1]) if seen else -1

        def observe(name):
            offset, before = logpath.stat().st_size, frames()
            # Wait for a fresh successful/published window on every channel.
            def ready():
                rows = perf_lines(logpath.read_bytes()[offset:].decode(errors='replace'))
                return len(rows) == 3 and all(int(w[-1]['completed']) > 0 and int(w[-1]['published']) > 0 for w in rows.values())
            wait_for(ready)
            assert client.poll() is None and frames() > before
            text = logpath.read_text(errors='replace')
            assert not re.search(r'model inference exception|FATAL|CRITICAL|job abort|failed to submit', text)
            report.append(dict(step=name, result='PASS', decoded_frames=frames()))
            print(name, 'PASS', flush=True)

        def change(config, expected):
            offset = logpath.stat().st_size
            save(config)
            wait_for(lambda: all(s in logpath.read_bytes()[offset:].decode(errors='replace') for s in expected))

        try:
            def listening():
                try:
                    with socket.create_connection(('127.0.0.1', 18554), timeout=.1):
                        return True
                except OSError:
                    return False
            wait_for(listening)
            client = subprocess.Popen(['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error',
                '-progress', 'pipe:1', '-rtsp_transport', 'tcp', '-i', 'rtsp://127.0.0.1:18554/live',
                '-an', '-vsync', '0', '-enc_time_base', '-1', '-f', 'null', '-'],
                stdout=video, stderr=subprocess.STDOUT)
            observe('fixed_startup')
            auto = copy.deepcopy(base)
            auto['channels'][0]['models'][0]['npu_core'] = -1
            change(auto, ['ch0 active automatic pool=0x1'])
            observe('first_auto')
            changed_pin = copy.deepcopy(auto)
            changed_pin['channels'][1]['models'][0]['npu_core'] = 0
            change(changed_pin, ['ch0 active automatic pool=0x2', 'ch1 model reload complete'])
            observe('unchanged_auto_follows_other_pin')
            all_auto = copy.deepcopy(base)
            for channel in all_auto['channels']:
                channel['models'][0]['npu_core'] = -1
            change(all_auto, [f'ch{i} active automatic pool=0x{1 << i:x}' for i in range(3)])
            observe('three_auto')
            change(base, [f'ch{i} model reload complete' for i in range(3)])
            observe('restore_fixed')

            # Existing but malformed file passes the path validator, then fails
            # rknn_init. The unrelated AUTO plan must follow the restored pin.
            invalid_model = root / 'invalid.rknn'
            invalid_model.write_bytes(b'Intentional invalid model for initialization rollback test\n')
            invalid = copy.deepcopy(changed_pin)
            invalid['channels'][1]['models'][0]['model_path'] = str(invalid_model)
            change(invalid, ['ch0 active automatic pool=0x2', 'ch1 model change rejected',
                             'ch0 active automatic pool=0x1'])
            observe('failed_pin_rolls_back_auto_pool')
            change(base, ['ch0 model reload complete'])
            observe('final_fixed')
        finally:
            try:
                stop(client, 5)
            finally:
                start = time.monotonic()
                stop(proc, 10)
                shutdown = time.monotonic() - start
        assert proc.returncode == 0 and 'Clean exit (code=0)' in logpath.read_text(errors='replace')
        (root / 'report.json').write_text(json.dumps(dict(steps=report, shutdown_seconds=shutdown), indent=2))


if __name__ == '__main__':
    main()
