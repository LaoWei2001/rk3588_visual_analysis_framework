"""Locate the platform library; source checkouts also work without installation."""
import importlib
import os
from pathlib import Path
import sys


def load_runtime(module: str):
    override = os.environ.get("RK3588_RUNTIME_LIB")
    source = Path(__file__).resolve().parents[3] / "services/framework/runtime_manager/python"
    installed = Path("/usr/lib/vision-analysis-platform/python")
    candidates = [Path(override)] if override else [source, installed]
    for directory in candidates:
        if (directory / "rk3588_runtime/__init__.py").is_file():
            if str(directory) not in sys.path:
                sys.path.insert(0, str(directory))
            return importlib.import_module("rk3588_runtime." + module)
    raise RuntimeError("设备运行平台尚未安装，请从项目根目录执行 install.sh")
