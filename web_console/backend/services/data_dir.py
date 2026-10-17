"""Compatibility import for the shared system runtime library."""
import sys
from .runtime_loader import load_runtime

_core = load_runtime("data_dir")

sys.modules[__name__] = _core
