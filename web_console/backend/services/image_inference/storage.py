"""Disk budget shared by image uploads, board-file copies and job monitoring."""
import shutil
from pathlib import Path

from .engine import JobError

MIN_FREE_BYTES = 100 * 1024 ** 2


def storage_info(root: Path) -> dict:
    existing = root
    while not existing.exists():
        existing = existing.parent
    free = shutil.disk_usage(existing).free
    return dict(free_bytes=free, min_free_bytes=MIN_FREE_BYTES,
                upload_available_bytes=max(0, free - MIN_FREE_BYTES - 1))


def require_space(root: Path, incoming: int = 0):
    info = storage_info(root)
    available = info['upload_available_bytes']
    if incoming > available or info['free_bytes'] <= MIN_FREE_BYTES:
        raise JobError(507, f'存储空间不足：需保留超过 100 MB，当前还可写入 {available / 1024**2:.1f} MB')
