"""Board A/B regression for global core placement with real video sources.

Use an idle RK3588 and an isolated application. Configs/logs can contain camera
credentials: the output directory is private and must be sanitized before sharing.
The first three base channels supply the primary comparison; the manifest supplies
six model nodes for the 1/2/4/6-channel and same-frame comparisons.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path

from run_npu_performance import run, comparison


CASES = ('fixed', 'first_auto', 'all_auto', 'one', 'two', 'four', 'six', 'composite')


def make_case(base, models, case):
    config = copy.deepcopy(base)
    config['global']['rtsp_port'] = 18554
    if case in ('fixed', 'first_auto', 'all_auto'):
        assert len(config['channels']) == 3
        for index, channel in enumerate(config['channels']):
            assert len(channel['models']) == 1
            channel['models'][0]['npu_core'] = -1 if case == 'all_auto' or (case == 'first_auto' and index == 0) else index
    else:
        count = dict(one=1, two=2, four=4, six=6, composite=1)[case]
        assert len(models) == 6 and len(base['channels']) >= 2
        config['channels'] = []
        for index in range(count):
            # One physical USB input; remaining logical channels reuse RTSP.
            channel = copy.deepcopy(base['channels'][1 if index == 1 else 0])
            group = models if case == 'composite' else [models[index]]
            channel.update(id=index, enable=True, models=copy.deepcopy(group))
            for model in channel['models']:
                model.update(enable=True, npu_core=-1)
            config['channels'].append(channel)
        config['global'].update(tile_rows=2 if count > 2 else 1,
                                tile_cols=3 if count > 4 else (2 if count > 1 else 1))
    return config


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('baseline', 'candidate', 'app', 'config', 'manifest', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--cases', nargs='+', choices=CASES, default=list(CASES))
    parser.add_argument('--warmup', type=int, default=15)
    parser.add_argument('--seconds', type=int, default=60)
    parser.add_argument('--repeats', type=int, default=2)
    args = parser.parse_args()
    assert args.seconds >= 30 and args.warmup >= 10 and args.repeats >= 1
    root = args.output.resolve()
    root.mkdir(mode=0o700, parents=True, exist_ok=False)
    os.environ.update(EVENT_STORE_DIR=str(root / 'events'), RK_LOGIC_CONTROL_SOCKET=str(root / 'control.sock'),
                      RK_FILE_PLAYBACK_CONTROL_SOCKET=str(root / 'playback.sock'), RK_DATASET_SOCKET=str(root / 'dataset.sock'))
    for key in ('NOTIFY_SOCKET', 'WATCHDOG_PID', 'WATCHDOG_USEC'):
        os.environ.pop(key, None)
    base, models = json.loads(args.config.read_text()), json.loads(args.manifest.read_text())
    binaries = dict(baseline=args.baseline.resolve(), candidate=args.candidate.resolve())
    report = dict(binaries={k: dict(sha256=digest(v)) for k, v in binaries.items()},
                  warmup_seconds=args.warmup, measurement_seconds=args.seconds, models=models, runs=[])
    for case in args.cases:
        config = make_case(base, models, case)
        for repeat in range(args.repeats):
            order = ('baseline', 'candidate') if repeat % 2 == 0 else ('candidate', 'baseline')
            for variant in order:
                directory = root / f'{case}-{repeat}-{variant}'
                directory.mkdir(mode=0o700)
                path = directory / 'config.private.json'
                path.write_text(json.dumps(config, indent=2))
                path.chmod(0o600)
                result = run(binaries[variant], args.app.resolve(), path, directory, args.warmup, args.seconds)
                report['runs'].append(dict(case=case, repeat=repeat, variant=variant, **result))
                (root / 'report.json').write_text(json.dumps(report, indent=2))
                (root / 'summary.json').write_text(json.dumps(comparison(report), indent=2))
    print('COMPARE PER-CHANNEL RESULTS:', root / 'summary.json', flush=True)
    # This report deliberately does not turn a heavy model's reduced core budget
    # into a claim that every FPS improved. Resource allocation is part of the result.


if __name__ == '__main__':
    main()
