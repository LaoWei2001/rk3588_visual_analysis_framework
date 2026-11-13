#!/usr/bin/env python3
"""Remove local logs/caches; optionally remove generated builds. Defaults to a dry run.

Never traverses assets, vendored dependencies, Git metadata or symlinks. Tracked
files are preserved when Git is available. Offline installation bundles are kept.
"""

import argparse
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
PROTECTED = {'assets', '.git', 'node_modules', 'vendor'}
CACHE_DIRS = {
    '__pycache__', '.pytest_cache', '.mypy_cache', '.ruff_cache',
    'Testing', 'test-results', 'playwright-report', 'htmlcov',
}
BUILD_DIRS = {
    'build', 'dist', 'web_console/frontend/dist',
    'engine/tests/performance/results', 'services/framework/gpio_state/build',
    'tools/device/gpio_test/build', 'tools/device/first_net_config/build',
}
BUILD_FILES = {
    'tools/device/first_net_config/first_net_config',
    'tools/device/npu_bench/npu_bench', 'tools/device/rknn_profile/rknn_profile',
}


def tracked_files(root):
    if not (root / '.git').exists():
        return set()
    result = subprocess.run(['git', '-C', str(root), 'ls-files', '-z'],
                            check=True, stdout=subprocess.PIPE)
    return set(os.fsdecode(result.stdout).split('\0')) - {''}


def collect_candidates(root, include_build=False):
    """Return files and emptyable directories in deletion order, without writing."""
    tracked = tracked_files(root)
    files, directories = [], []

    def visit(path, purge=False):
        relative = path.relative_to(root).as_posix()
        if path.name in PROTECTED or path.is_symlink():
            return True
        if path.is_dir():
            test_build = relative.startswith('engine/tests/') and path.name == 'build'
            purge = purge or path.name in CACHE_DIRS or (
                include_build and (relative in BUILD_DIRS or test_build))
            survives = False
            for child in sorted(path.iterdir()):
                if visit(child, purge):
                    survives = True
            if purge and not survives:
                directories.append(path)
                return False
            return True
        if not path.is_file() or relative in tracked:
            return True
        name = path.name
        disposable = (
            name.endswith(('.log', '.pyc', '.pyo', '.tsbuildinfo')) or '.log.' in name
            or name in {'.coverage', 'coverage.xml'} or name.startswith('.coverage.')
        )
        if purge or disposable or (include_build and relative in BUILD_FILES):
            files.append(path)
            return False
        return True

    for child in sorted(root.iterdir()):
        visit(child)
    return files, directories


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true', help='Delete the listed generated files')
    parser.add_argument('--build', action='store_true', help='Also clean build/package outputs, preserving assets')
    parser.add_argument('--verbose', action='store_true', help='List every selected path')
    args = parser.parse_args()
    files, directories = collect_candidates(ROOT, args.build)
    size = sum(path.stat().st_size for path in files)
    for path, is_directory in [(path, False) for path in files] + [(path, True) for path in directories]:
        if args.verbose:
            print(path.relative_to(ROOT))
        if args.apply:
            # Do not follow a symlink introduced after collecting candidates.
            if path.is_symlink():
                raise RuntimeError(f'Path became a symlink; stopped cleaning: {path}')
            if is_directory:
                path.rmdir()
            else:
                path.unlink()
    action = 'Removed' if args.apply else 'Would remove'
    print(f'{action} {len(files)} files and {len(directories)} directories ({size / 1024 / 1024:.2f} MiB).')
    if not args.apply:
        print('Dry run only. Add --apply to delete; add --verbose to list paths.')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f'Cleanup failed: {error}', file=sys.stderr)
        sys.exit(1)
