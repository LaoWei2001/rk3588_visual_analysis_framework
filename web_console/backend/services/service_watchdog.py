"""Compatibility import for the shared system runtime library."""
import sys
from .runtime_loader import load_runtime

_core = load_runtime("service_watchdog")

sys.modules[__name__] = _core
