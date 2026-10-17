"""
log_buffer.py — 每个 App 独立的内存日志环形缓冲区，支持 WebSocket 订阅推送。

设计原则：
  - 本模块只维护内存缓冲；视觉服务的原始日志由 systemd journald 管理。
  - reader 线程（process_manager 里的 _consume_logs）通过 push() 把行写入 deque，
    同时通过 asyncio.run_coroutine_threadsafe 把行投递到所有已订阅的 asyncio.Queue。
  - WebSocket handler 在 subscribe() 时捕获当前事件循环（async 上下文），
    此后 push() 即可跨线程推送。
"""

import asyncio
import collections
import threading
from typing import Dict, Optional, Set

MAX_LINES = 1000
MAX_LINE_CHARS = 2048
MAX_SUBSCRIBER_LINES = 256
MAX_BUFFERS = 16


class AppLogBuffer:
    def __init__(self) -> None:
        self._lines: collections.deque = collections.deque(maxlen=MAX_LINES)
        self._subscribers: Set[asyncio.Queue] = set()
        self._lock = threading.Lock()
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._pending: collections.deque = collections.deque(maxlen=MAX_SUBSCRIBER_LINES)
        self._dispatch_scheduled = False

    # ── Called from reader thread ────────────────────────────────────────────
    def push(self, line: str) -> None:
        line = line[:MAX_LINE_CHARS]
        with self._lock:
            self._lines.append(line)
            loop = self._loop
            schedule = False
            if self._subscribers and loop and loop.is_running():
                self._pending.append(line)
                if not self._dispatch_scheduled:
                    self._dispatch_scheduled = schedule = True
        if schedule and loop:
            try:
                loop.call_soon_threadsafe(self._flush)
            except RuntimeError:  # The browser event loop may close concurrently.
                with self._lock:
                    self._dispatch_scheduled = False
                    self._pending.clear()

    def _flush(self) -> None:
        with self._lock:
            lines = list(self._pending)
            self._pending.clear()
            self._dispatch_scheduled = False
            subscribers = list(self._subscribers)
        for q in subscribers:
            for line in lines:
                self._offer(q, line)

    @staticmethod
    def _offer(q: asyncio.Queue, line: str) -> None:
        # Never await a full subscriber queue or accumulate pending put tasks.
        while q.qsize() >= MAX_SUBSCRIBER_LINES or q.full():
            try:
                q.get_nowait()
            except asyncio.QueueEmpty:
                break
        q.put_nowait(line)

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
            if len(_buffers) >= MAX_BUFFERS:
                unused = next((name for name, buffer in _buffers.items() if not buffer._subscribers), None)
                if unused is not None:
                    del _buffers[unused]
            _buffers[app_name] = AppLogBuffer()
        return _buffers[app_name]
