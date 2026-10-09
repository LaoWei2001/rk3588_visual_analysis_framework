"""Build receiver downloads without accessing task state or receiver leases."""
import hashlib
import io
import json
import struct
import zipfile
from functools import lru_cache
from pathlib import Path
from urllib.parse import urlparse

RESOURCES = Path(__file__).resolve().parents[1] / "resources"
WINDOWS_FILENAME = "dataset-receiver-windows-amd64.exe"


def _stamp(path: Path) -> tuple:
    stat = path.stat()
    return (str(path), stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns)


@lru_cache(maxsize=4)
def _validate(binary_stamp: tuple, manifest_stamp: tuple, source_stamp: tuple) -> None:
    binary, manifest, source = (Path(stamp[0]) for stamp in (binary_stamp, manifest_stamp, source_stamp))
    info = json.loads(manifest.read_text(encoding="utf-8"))
    if not isinstance(info, dict) or info.get("format") != 1 or info.get("platform") != "windows-amd64":
        raise ValueError("Windows 接收程序版本信息无效，请重新构建")
    if info.get("receiver_sha256") != hashlib.sha256(source.read_bytes()).hexdigest():
        raise ValueError("接收程序源码已更新，请重新构建 Windows EXE")
    digest = hashlib.sha256()
    with binary.open("rb") as handle:
        if handle.read(2) != b"MZ":
            raise ValueError("Windows 接收文件不是 EXE")
        handle.seek(0x3c)
        offset_bytes = handle.read(4)
        if len(offset_bytes) != 4:
            raise ValueError("Windows EXE 文件不完整")
        handle.seek(struct.unpack("<I", offset_bytes)[0])
        if handle.read(6) != b"PE\x00\x00\x64\x86":
            raise ValueError("接收程序必须为 Windows 64 位 EXE")
        handle.seek(0)
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    if digest.hexdigest() != info.get("sha256"):
        raise ValueError("Windows EXE 校验失败，请重新构建或重新部署")


def windows_receiver() -> Path:
    directory = RESOURCES / "receiver" / "windows-amd64"
    binary = directory / "dataset_receiver.exe"
    manifest = directory / "manifest.json"
    source = RESOURCES / "dataset_receiver.py"
    try:
        _validate(_stamp(binary), _stamp(manifest), _stamp(source))
    except FileNotFoundError as exc:
        raise ValueError("尚未部署 Windows EXE 接收程序，请先构建并部署电脑接收端") from exc
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"无法读取 Windows 接收程序: {exc}") from exc
    return binary


def normalize_receiver_url(url: str) -> str:
    url = url.rstrip('/')
    parsed = urlparse(url)
    if url and (
        parsed.scheme not in ('http', 'https') or not parsed.hostname
        or parsed.username or parsed.password or parsed.query or parsed.fragment
    ):
        raise ValueError('请填写浏览器访问控制台的 HTTP/HTTPS 地址')
    return url


def python_receiver(url: str) -> bytes:
    output = io.BytesIO()
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.write(RESOURCES / 'dataset_receiver.py', 'dataset_receiver.py')
        archive.writestr('receiver.json', json.dumps({'url': url}, ensure_ascii=False, indent=2))
        archive.writestr('启动接收.cmd', (
            '@echo off\r\ncd /d "%~dp0"\r\npy -3 dataset_receiver.py\r\n'
            'if errorlevel 9009 python dataset_receiver.py\r\npause\r\n'
        ))
        archive.writestr('start.sh', '#!/bin/sh\ncd "$(dirname "$0")"\nexec python3 dataset_receiver.py "$@"\n')
        archive.writestr('使用说明.txt', (
            '需要 Python 3.9 或更高版本，无需 pip 安装依赖。\n'
            'Windows 双击“启动接收.cmd”；Linux/macOS 执行 python3 dataset_receiver.py。\n'
            '首次填写设备地址、选择电脑保存目录；也可通过 --url 和 --output 指定。无需账号或接收凭据。\n'
            '一个接收程序自动接收本设备的全部采集任务，新增或修改任务无需重新下载。\n'
            '可关闭浏览器和 SSH，但不能关闭接收程序。断网自动重连，电脑需保持开机、不休眠。\n'
            '更换电脑先正常退出原接收程序；离线通知成功即可更换，异常退出需等待30秒。\n'
            'Web 当前存图按所选目录中的设备/程序/通道 JPG 文件统计，手动删图后自动更新。\n'
            '接收程序运行时后台每轮统计后等待5秒再检查；大目录需要更多时间。累计采集上限不因删图重置。\n'
            '采集接口免登录，能访问设备采集接口的电脑均可连接；任务配置等控制台功能仍需登录。\n'
        ))
    return output.getvalue()
