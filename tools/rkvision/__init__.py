"""RKVision unified command-line program."""

__all__ = ["__version__"]


def __version__() -> str:
    from .common import ENGINE_ROOT

    return (ENGINE_ROOT / "VERSION").read_text(encoding="utf-8").strip()
