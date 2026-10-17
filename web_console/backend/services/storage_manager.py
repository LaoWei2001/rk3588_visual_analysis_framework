"""Compatibility import for the shared system runtime library."""
import sys
from .runtime_loader import load_runtime

_core = load_runtime("storage_manager")

sys.modules[__name__] = _core
