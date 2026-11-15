#!/usr/bin/env python3
"""Run the representative audit regression suites from any working directory.

Dependencies must already be installed. Hardware inference is excluded; this
command does not install packages, publish applications or restart services.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
FRONTEND = ROOT / 'web_console/frontend'
BACKEND_TESTS = [
    'test_audit_regressions.py', 'test_config_io.py', 'test_assets.py',
    'test_logic_control.py', 'test_record_export.py', 'test_model_info.py',
    'test_config_sync_reads.py', 'test_records_split_event.py', 'test_alarm_retention.py',
]
BROWSER_TESTS = [
    'test_terminal_connection.py', 'test_terminal_display.py',
    'test_live_view_logs.py', 'test_web_config_sync.py',
    'test_terminal_transport_rendering.py', 'test_terminal_recess.py',
]
FRONTEND_TESTS = [
    'test_api_transport.py', 'test_terminal_transport.py', 'test_channel_config.py',
]
CPP_OPTIONS = [
    'MODEL_METADATA', 'NPU', 'DEPTH', 'SDK', 'INFERENCE_ROI',
    'RTSP', 'FRAME_SNAPSHOT', 'HELMET', 'PPE',
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--suite', nargs='+', choices=['backend', 'frontend', 'cpp', 'browser'],
                        default=['backend', 'frontend', 'cpp'])
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/engine')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--dry-run', action='store_true', help='Print commands without executing them')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    env = os.environ.copy()
    env['PYTHONPATH'] = os.pathsep.join(filter(None, [str(ROOT / 'web_console/backend'), env.get('PYTHONPATH')]))

    def run(command, cwd=ROOT):
        command = [str(arg) for arg in command]
        print(f'[{cwd.relative_to(ROOT) if cwd.is_relative_to(ROOT) else cwd}] {shlex.join(command)}', flush=True)
        if not args.dry_run:
            subprocess.run(command, cwd=cwd, env=env, check=True)

    def pytest(directory, files):
        run([sys.executable, '-m', 'pytest', '-q', '-x', *[directory / file for file in files]])

    suites = set(args.suite)
    if 'backend' in suites:
        pytest(ROOT / 'web_console/backend/tests', BACKEND_TESTS)
    if suites & {'frontend', 'browser'}:
        run(['npm', 'run', 'build'], FRONTEND)
    if 'frontend' in suites:
        pytest(FRONTEND / 'tests', FRONTEND_TESTS)
    if 'browser' in suites:
        pytest(FRONTEND / 'tests', BROWSER_TESTS)
    if 'cpp' in suites:
        build = args.build_dir.resolve()
        run(['cmake', '-S', ROOT / 'engine', '-B', build,
             *[f'-DBUILD_{option}_TESTS=ON' for option in CPP_OPTIONS]])
        run(['cmake', '--build', build, f'-j{args.jobs}'])
        # Older board CTest has no --test-dir support; cwd must be the build dir.
        run(['ctest', '--output-on-failure', '-LE', 'hardware'], build)


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
