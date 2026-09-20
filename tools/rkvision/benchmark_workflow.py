"""Alternate two vision binaries against the same configuration on an idle RK3588."""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import statistics
import subprocess
import time


def measure(binary, config, seconds, output, label, trial, cwd, warmup_seconds=5):
    log = output / f'{trial}-{label}.log'
    samples = []
    with log.open('w') as stream:
        proc = subprocess.Popen([str(binary), str(config)], cwd=cwd,
                                stdout=stream, stderr=subprocess.STDOUT)
        start = time.monotonic()
        try:
            while proc.poll() is None and time.monotonic() - start < seconds:
                try:
                    fields = Path(f'/proc/{proc.pid}/stat').read_text().split()
                    resident = int(Path(f'/proc/{proc.pid}/statm').read_text().split()[1]) * os.sysconf('SC_PAGE_SIZE')
                    samples.append((time.monotonic(), int(fields[13]) + int(fields[14]), resident))
                except FileNotFoundError:
                    pass
                time.sleep(1)
        finally:
            if proc.poll() is None:
                proc.send_signal(signal.SIGINT)
                try:
                    proc.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
    readings = {}
    for ch, fps, total in re.findall(r'\[Perf\]\[ch(\d+)\]\[5s\] fps=([\d.]+).*?total=([\d.]+)ms', log.read_text(errors='replace')):
        readings.setdefault(ch, []).append((float(fps), float(total)))
    values = [value for channel in readings.values() for value in channel[warmup_seconds // 5:]]
    steady_samples = [sample for sample in samples if sample[0] - start >= warmup_seconds]
    if not values or len(steady_samples) < 2 or proc.returncode != 0:
        raise RuntimeError(f'没有获得有效的完整测量：{log}；退出码={proc.returncode}')
    return {
        'label': label, 'trial': trial, 'channels': len(readings), 'samples': len(values),
        'fps_per_channel': statistics.mean(value[0] for value in values),
        'inference_total_ms': statistics.mean(value[1] for value in values),
        'cpu_percent': (steady_samples[-1][1] - steady_samples[0][1]) / os.sysconf('SC_CLK_TCK') / (steady_samples[-1][0] - steady_samples[0][0]) * 100,
        'rss_mb': statistics.mean(value[2] for value in steady_samples) / 1024**2,
        'warmup_seconds': warmup_seconds,
        'exit_code': proc.returncode, 'log': str(log),
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cwd', type=Path, default=Path.cwd())
    parser.add_argument('--seconds', type=int, default=32)
    parser.add_argument('--warmup-seconds', type=int, default=5,
                        help='exclude initial warmup; positive multiple of the five-second performance window')
    args = parser.parse_args(argv)
    if args.seconds < 20:
        raise ValueError('测量时间至少需要 20 秒，以采集稳态样本')
    if args.warmup_seconds < 5 or args.warmup_seconds % 5 or args.seconds < args.warmup_seconds + 15:
        raise ValueError('预热时间必须是 5 秒的正整数倍，并且预热后至少保留 15 秒测量时间')
    args.output.mkdir(parents=True, exist_ok=False)
    records = []
    for index, label in enumerate(('baseline', 'refactor', 'refactor', 'baseline')):
        binary = args.before if label == 'baseline' else args.after
        record = measure(binary.resolve(), args.config.resolve(), args.seconds, args.output, label, index, args.cwd,
                         args.warmup_seconds)
        records.append(record)
        (args.output / 'results.json').write_text(json.dumps(records, indent=2) + '\n')
        print(json.dumps(record), flush=True)
    return 0


if __name__ == '__main__':
    main()
