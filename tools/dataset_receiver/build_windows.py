#!/usr/bin/env python3
"""Cross-build a standalone Windows receiver, including its private Python runtime."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "web_console/backend/resources/dataset_receiver.py"
OUTPUT = ROOT / "web_console/backend/resources/receiver/windows-amd64"
PYTHON_VERSION = "3.13.16"
RUNTIME_URL = f"https://www.python.org/ftp/python/{PYTHON_VERSION}/python-{PYTHON_VERSION}-embed-amd64.zip"
# Official SHA-256: https://www.python.org/downloads/release/python-31316/
RUNTIME_SHA256 = "97dae5274cc54867065e8d5a3226e48c35017ed332a0fdb0e27d5b5821961297"


def checked_runtime(path: Path) -> None:
    if hashlib.sha256(path.read_bytes()).hexdigest() != RUNTIME_SHA256:
        raise ValueError("Python 运行时 SHA-256 校验失败")
    with zipfile.ZipFile(path) as archive:
        required = {"python.exe", "python313.dll", "python313.zip", "python313._pth",
                    "_ssl.pyd", "_sqlite3.pyd", "sqlite3.dll", "_ctypes.pyd", "libffi-8.dll", "LICENSE.txt"}
        if not required.issubset(archive.namelist()):
            raise ValueError("Python 嵌入式运行时不完整")


def build(output: Path, runtime_zip: Path, go_binary: str, run_tests: bool = True) -> Path:
    checked_runtime(runtime_zip)
    output.mkdir(parents=True, exist_ok=True)
    launcher = Path(__file__).parent
    with tempfile.TemporaryDirectory(prefix="dataset-receiver-build-") as temporary:
        work = Path(temporary)
        for source in [launcher / "go.mod", *launcher.glob("*.go")]:
            shutil.copyfile(source, work / source.name)
        payload = work / "payload"
        payload.mkdir()
        shutil.copyfile(runtime_zip, payload / "python-runtime.zip")
        shutil.copyfile(SOURCE, payload / "dataset_receiver.py")
        compile((payload / "dataset_receiver.py").read_bytes(), str(SOURCE), "exec")
        go_root = Path(subprocess.check_output([go_binary, "env", "GOROOT"], text=True).strip())
        shutil.copyfile(go_root / "LICENSE", payload / "GO-LICENSE.txt")
        env = os.environ.copy()
        env.update(CGO_ENABLED="0", GOTOOLCHAIN="local")
        if run_tests:
            subprocess.run([go_binary, "test", "-p", "2", "./..."], cwd=work, env=env, check=True)
        env.update(GOOS="windows", GOARCH="amd64")
        binary = work / "dataset_receiver.exe"
        subprocess.run([go_binary, "build", "-p", "2", "-trimpath", "-buildvcs=false",
                        "-ldflags=-s -w", "-o", str(binary), "."], cwd=work, env=env, check=True)
        with zipfile.ZipFile(runtime_zip) as archive:
            (work / "PYTHON-LICENSE.txt").write_bytes(archive.read("LICENSE.txt"))
        shutil.copyfile(payload / "GO-LICENSE.txt", work / "GO-LICENSE.txt")
        manifest = {"format": 1, "platform": "windows-amd64", "python_version": PYTHON_VERSION,
                    "runtime_url": RUNTIME_URL, "runtime_sha256": RUNTIME_SHA256,
                    "receiver_sha256": hashlib.sha256((payload / 'dataset_receiver.py').read_bytes()).hexdigest(),
                    "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                    "go_version": subprocess.check_output([go_binary, "version"], text=True).strip()}
        (work / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        for name in ("dataset_receiver.exe", "PYTHON-LICENSE.txt", "GO-LICENSE.txt", "manifest.json"):
            temporary_output = output / (name + ".tmp")
            shutil.copyfile(work / name, temporary_output)
            temporary_output.replace(output / name)
    return output / "dataset_receiver.exe"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--runtime-zip", type=Path, help="已下载的官方运行时（支持离线构建）")
    parser.add_argument("--go", default=shutil.which("go"), help="Go 编译器路径；使用 Go 1.23 或更高版本")
    args = parser.parse_args()
    if not args.go:
        parser.error("构建机需要 Go 编译器；使用 EXE 的电脑不需要 Go 或 Python")
    with tempfile.TemporaryDirectory(prefix="dataset-python-runtime-") as temporary:
        runtime_zip = args.runtime_zip
        if runtime_zip is None:
            runtime_zip = Path(temporary) / "python-runtime.zip"
            print(f"下载内置运行时：{RUNTIME_URL}", flush=True)
            with urllib.request.urlopen(RUNTIME_URL, timeout=60) as response, runtime_zip.open("wb") as output:
                shutil.copyfileobj(response, output)
        binary = build(args.output.resolve(), runtime_zip.resolve(), args.go)
        print(f"已生成：{binary} ({binary.stat().st_size / 1024 ** 2:.1f} MB)", flush=True)


if __name__ == "__main__":
    main()
