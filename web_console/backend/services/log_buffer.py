"""
log_buffer.py — 每个 App 独立的内存日志环形缓冲区，支持 WebSocket 订阅推送。

设计原则：
  - 不写任何磁盘文件；所有日志仅存活于本次 Web 控制台进程的内存中。
  - reader 线程（process_manager 里的 _pipe_reader）通过 push() 把行写入 deque，
    同时通过 asyncio.run_coroutine_threadsafe 把行投递到所有已订阅的 asyncio.Queue。
  - WebSocket handler 在 subscribe() 时捕获当前事件循环（async 上下文），
    此后 push() 即可跨线程推送。
"""

import asyncio
import collections
import threading
from typing import Dict, Optional, Set

MAX_LINES = 5000  # 每个 App 最多保留的行数


class AppLogBuffer:
    def __init__(self) -> None:
        self._lines: collections.deque = collections.deque(maxlen=MAX_LINES)
        self._subscribers: Set[asyncio.Queue] = set()
        self._lock = threading.Lock()
        self._loop: Optional[asyncio.AbstractEventLoop] = None

    # ── Called from reader thread ────────────────────────────────────────────
    def push(self, line: str) -> None:
        with self._lock:
            self._lines.append(line)
            subs = list(self._subscribers)
            loop = self._loop
        if loop and loop.is_running():
            for q in subs:
                asyncio.run_coroutine_threadsafe(q.put(line), loop)

    # ── Called from async WebSocket handler ─────────────────────────────────
    def subscribe(self, q: asyncio.Queue) -> None:
        """Register a queue to receive future push() calls.
        Must be called from an async context so we can capture the running loop.
        """
        with self._lock:
            self._subscribers.add(q)
        try:
            self._loop = asyncio.get_running_loop()
        except RuntimeError:
            pass

    def unsubscribe(self, q: asyncio.Queue) -> None:
        with self._lock:
            self._subscribers.discard(q)

    # ── General helpers ──────────────────────────────────────────────────────
    def get_tail(self, n: int = 300) -> list:
        with self._lock:
            return list(self._lines)[-n:]

    def clear(self) -> None:
        with self._lock:
            self._lines.clear()


# ── Module-level registry ────────────────────────────────────────────────────
_buffers: Dict[str, AppLogBuffer] = {}
_registry_lock = threading.Lock()


def get_log_buffer(app_name: str) -> AppLogBuffer:
    """Return (or lazily create) the AppLogBuffer for a given app."""
    with _registry_lock:
        if app_name not in _buffers:
            _buffers[app_name] = AppLogBuffer()
        return _buffers[app_name]
