"""Bound only operational diagnostics; never remove business media or configs."""
from pathlib import Path

from .data_dir import APPS_ROOT


def prune_files(paths, keep, max_bytes):
    files = []
    for path in paths:
        if path.is_symlink() or not path.is_file():
            continue
        info = path.stat()
        files.append((info.st_mtime_ns, path.name, info.st_size, path))
    retained_bytes = 0
    retained_count = 0
    removed = 0
    for _, _, size, path in sorted(files, reverse=True):
        if retained_count < keep and retained_bytes + size <= max_bytes:
            retained_count += 1
            retained_bytes += size
        else:
            path.unlink(missing_ok=True)
            removed += 1
    return removed


def cleanup(pstore=Path('/var/lib/systemd/pstore'), apps_root=None):
    removed = 0
    from . import recovery_history
    with recovery_history.locked():
        removed += prune_files(recovery_history.HISTORY_FILE.parent.glob('.recovery-history-*'), keep=0, max_bytes=0)
    if pstore.is_dir() and not pstore.is_symlink():
        # Only systemd's archived diagnostics; never modify the kernel pstore.
        removed += prune_files([p for p in pstore.iterdir() if p.name.startswith(('console-', 'dmesg-', 'pmsg-', 'ftrace-'))],
                               keep=3, max_bytes=3 * 1024 * 1024)
    root = APPS_ROOT if apps_root is None else apps_root
    if root.is_dir():
        for app in root.iterdir():
            if app.name.startswith(('.', '_')) or app.is_symlink() or not app.is_dir():
                continue
            cores = []
            for path in app.glob('core*'):
                if path.is_symlink() or not path.is_file():
                    continue
                with path.open('rb') as handle:
                    header = handle.read(18)
                # Only verified ELF ET_CORE files, not files merely named core*.
                if (len(header) == 18 and header[:4] == b'\x7fELF' and header[5] in (1, 2)
                        and int.from_bytes(header[16:18], 'little' if header[5] == 1 else 'big') == 4):
                    cores.append(path)
            removed += prune_files(cores, keep=3, max_bytes=32 * 1024 * 1024)
    return removed
