"""Compatibility import for the shared system runtime library."""
import sys
from .runtime_loader import load_runtime

_core = load_runtime("process_manager")

def _pause_dataset(app_name):
    from services.dataset_manager import dataset_manager
    dataset_manager.pause_app(app_name)

_core.before_stop = _pause_dataset

sys.modules[__name__] = _core
