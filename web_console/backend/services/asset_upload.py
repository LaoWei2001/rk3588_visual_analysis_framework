"""Publish a complete uploaded file without replacing existing assets by default."""
import os
from pathlib import Path
import shutil
import tempfile
from typing import BinaryIO


def save_asset(directory: Path, fname: str, source: BinaryIO, overwrite: bool = False) -> Path:
    # A private staging file plus atomic hard-link publication makes the
    # no-overwrite contract safe across threads and worker processes.
    tmp = None
    target = directory / fname
    try:
        with tempfile.NamedTemporaryFile(dir=directory, prefix=".upload_", suffix=".tmp", delete=False) as out:
            tmp = Path(out.name)
            source.seek(0)
            shutil.copyfileobj(source, out)
        if overwrite:
            os.replace(tmp, target)
        else:
            stem, ext = Path(fname).stem, Path(fname).suffix
            index = 0
            while True:
                try:
                    os.link(tmp, target)
                    break
                except FileExistsError:
                    index += 1
                    suffix = "_copy" if index == 1 else f"_copy_{index}"
                    target = directory / f"{stem}{suffix}{ext}"
        return target
    finally:
        if tmp is not None:
            tmp.unlink(missing_ok=True)
