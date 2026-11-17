"""Discover board images and snapshot them into an existing inference batch."""
from dataclasses import dataclass
import os
from pathlib import Path
import stat
from typing import Callable, List, Tuple

from .engine import IMAGE_EXTENSIONS, JobError
from .storage import require_space


def identity(info: os.stat_result) -> Tuple[int, int, int, int]:
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns


@dataclass(frozen=True)
class DeviceImage:
    path: Path
    name: str
    version: Tuple[int, int, int, int]

    @property
    def size(self) -> int:
        return self.version[2]


@dataclass(frozen=True)
class DeviceSelection:
    path: Path
    is_directory: bool
    recursive: bool
    images: List[DeviceImage]

    def source(self) -> dict:
        return dict(kind='device', path=str(self.path), is_directory=self.is_directory, recursive=self.recursive)

    def preview(self) -> dict:
        return dict(**self.source(), total=len(self.images), total_bytes=sum(image.size for image in self.images),
                    images=[dict(name=image.name, path=str(image.path), size=image.size) for image in self.images])


def scan_device_images(value: str, recursive: bool = False) -> DeviceSelection:
    if not isinstance(value, str) or not value.strip() or len(value) > 4096 or '\0' in value:
        raise JobError(422, '请输入 RK3588 上的图片或文件夹绝对路径')
    if not isinstance(recursive, bool):
        raise JobError(422, '是否包含子文件夹必须是布尔值')
    requested = Path(value.strip())
    if not requested.is_absolute():
        raise JobError(422, '板端路径需要以 / 开头，例如 /userdata/pictures')
    try:
        root = requested.resolve(strict=True)
        info = root.stat()
        directory = stat.S_ISDIR(info.st_mode)
        if not directory and not stat.S_ISREG(info.st_mode):
            raise JobError(422, '仅支持普通图片文件或文件夹')
        images = []

        def add(path: Path, name: str):
            if path.suffix.lower() not in IMAGE_EXTENSIONS:
                raise JobError(422, '支持 JPG、PNG、BMP、WebP 图片')
            resolved = path.resolve(strict=True)
            file_info = resolved.stat()
            if not stat.S_ISREG(file_info.st_mode):
                raise JobError(422, f'不是普通图片文件：{path}')
            if file_info.st_size <= 0:
                raise JobError(422, f'图片为空：{path}')
            images.append(DeviceImage(resolved, name, identity(file_info)))

        if directory:
            pending, visited = [root], set()
            while pending:
                folder = pending.pop()
                folder_info = folder.stat()
                key = (folder_info.st_dev, folder_info.st_ino)
                if key in visited:
                    continue
                visited.add(key)
                with os.scandir(folder) as entries:
                    for entry in entries:
                        path = Path(entry.path)
                        if entry.is_dir(follow_symlinks=False):
                            if recursive:
                                pending.append(path)
                        elif path.suffix.lower() in IMAGE_EXTENSIONS and entry.is_file():
                            add(path, str(path.relative_to(root)))
            images.sort(key=lambda image: (image.name.casefold(), image.name))
            if not images:
                raise JobError(422, '文件夹中没有支持的图片；如图片位于下级目录，请勾选「包括子文件夹」')
        else:
            add(requested, requested.name)
        return DeviceSelection(requested, directory, recursive if directory else False, images)
    except JobError:
        raise
    except (OSError, RuntimeError) as exc:
        raise JobError(422, f'无法读取板端路径：{value}（{exc}）') from exc


def snapshot_device_image(image: DeviceImage, target: Path, checkpoint: Callable[[], None]):
    """Read sources only. A private copy keeps retries and task deletion independent."""
    checkpoint()
    try:
        # NONBLOCK prevents a file replaced by a FIFO from hanging a request.
        descriptor = os.open(image.path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
        with os.fdopen(descriptor, 'rb') as source:
            before = os.fstat(source.fileno())
            if not stat.S_ISREG(before.st_mode) or identity(before) != image.version:
                raise JobError(409, f'图片在读取前发生了变化，请重试：{image.path}')
            copied = 0
            with target.open('xb') as output:
                while True:
                    checkpoint()
                    chunk = source.read(1024 * 1024)
                    if not chunk:
                        break
                    copied += len(chunk)
                    if copied > image.size:
                        raise JobError(409, f'图片在读取时发生了变化，请重试：{image.path}')
                    require_space(target.parent, len(chunk))
                    output.write(chunk)
                    output.flush()
                    require_space(target.parent)
            if copied != image.size or identity(os.fstat(source.fileno())) != image.version:
                raise JobError(409, f'图片在读取时发生了变化，请重试：{image.path}')
    except OSError as exc:
        raise JobError(422, f'复制板端图片失败：{image.path}（{exc.strerror}）') from exc
