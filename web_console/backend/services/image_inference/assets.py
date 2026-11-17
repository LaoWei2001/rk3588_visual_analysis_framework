"""Console model library. Uploaded files belong to no video application."""
import os
from pathlib import Path
import shutil
import tempfile
from typing import BinaryIO

from .engine import JobError

MAX_MODEL_BYTES = 1024 ** 3
MAX_LABEL_BYTES = 4 * 1024 ** 2
MIN_FREE_BYTES = 128 * 1024 ** 2


class ModelAssets:
    def __init__(self, root: Path):
        self.root = root.resolve()

    def list(self) -> dict:
        result = dict(models=[], labels=[])
        if self.root.is_dir():
            for path in sorted(self.root.iterdir()):
                kind = {'.rknn': 'models', '.txt': 'labels'}.get(path.suffix.lower())
                if kind and path.is_file() and not path.is_symlink():
                    result[kind].append(str(path))
        return result

    def save(self, filename: str, source: BinaryIO) -> dict:
        name = Path((filename or '').replace('\\', '/')).name
        suffix = Path(name).suffix.lower()
        limit = {'.rknn': MAX_MODEL_BYTES, '.txt': MAX_LABEL_BYTES}.get(suffix)
        if not name or '\0' in name or len(name.encode('utf-8')) > 240 or limit is None:
            raise JobError(422, '仅支持 .rknn 模型或 .txt 标签文件，文件名最多 240 字节')
        self.root.mkdir(parents=True, exist_ok=True)
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=self.root, prefix='.import-', suffix='.tmp', delete=False) as target:
                temporary = Path(target.name)
                size = 0
                source.seek(0)
                while True:
                    chunk = source.read(1024 * 1024)
                    if not chunk:
                        break
                    size += len(chunk)
                    if size > limit:
                        raise JobError(413, '模型限 1 GiB，标签文件限 4 MiB')
                    if shutil.disk_usage(self.root).free < MIN_FREE_BYTES + len(chunk):
                        raise JobError(507, '模型库剩余空间不足')
                    target.write(chunk)
            if size == 0:
                raise JobError(422, '不能导入空文件')
            path = self.root / name
            index = 0
            while True:
                try:
                    os.link(temporary, path)
                    break
                except FileExistsError:
                    index += 1
                    path = self.root / f'{Path(name).stem}_{index}{suffix}'
            return dict(path=str(path), name=path.name, renamed=path.name != name, size=size)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
