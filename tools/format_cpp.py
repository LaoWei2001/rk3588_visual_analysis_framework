#!/usr/bin/env python3
"""Format project C/C++ sources with the repository's .clang-format."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = (
    ROOT / 'engine/src', ROOT / 'engine/include', ROOT / 'engine/tests',
    ROOT / 'projects/modules', ROOT / 'projects/global_modules',
)
EXCLUDED = {'assets', 'vendor', 'node_modules', '.git', 'build', 'dist', 'generated'}
EXTENSIONS = {'.c', '.cpp', '.h', '.hpp'}


def source_files(paths):
    files = set()
    for value in paths:
        path = Path(value).absolute()
        if not path.exists():
            raise ValueError(f'Path does not exist: {value}')
        # Resolve before checking the boundary so symlinks cannot escape it.
        path = path.resolve()
        if not any(path == root or root in path.parents for root in SOURCE_ROOTS):
            raise ValueError(f'Path is outside the project C/C++ source roots: {value}')
        if EXCLUDED.intersection(path.relative_to(ROOT).parts):
            raise ValueError(f'Protected directory: {value}')
        if path.is_file():
            if path.suffix not in EXTENSIONS:
                raise ValueError(f'Not a C/C++ source: {value}')
            files.add(path)
            continue
        for base, directories, names in os.walk(path, followlinks=False):
            directories[:] = [name for name in directories
                              if name not in EXCLUDED and not (Path(base) / name).is_symlink()]
            for name in names:
                candidate = Path(base) / name
                if candidate.suffix in EXTENSIONS and not candidate.is_symlink():
                    files.add(candidate)
    return sorted(files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('paths', nargs='*', help='Files or directories; defaults to project source roots')
    parser.add_argument('--check', action='store_true', help='Report formatting differences without writing')
    args = parser.parse_args()
    try:
        files = source_files(args.paths or SOURCE_ROOTS)
    except ValueError as error:
        parser.error(str(error))
    formatter = shutil.which('clang-format')
    if not formatter:
        parser.error('clang-format is not installed')
    differences = 0
    for path in files:
        if args.check:
            formatted = subprocess.check_output([formatter, '-style=file', str(path)])
            if formatted != path.read_bytes():
                print(path.relative_to(ROOT))
                differences += 1
        else:
            subprocess.run([formatter, '-i', '-style=file', str(path)], check=True)
    print(f'{len(files)} source files {"checked" if args.check else "formatted"}; '
          f'{differences} formatting differences')
    return 1 if differences else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
