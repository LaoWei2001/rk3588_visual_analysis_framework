import asyncio

from fastapi import WebSocketDisconnect
from services import log_streamer


def test_logs_are_batched_in_order_and_stop_is_delivered(monkeypatch):
    messages = []
    rows = [f'line-{i}' for i in range(150)] + ['[进程已停止]']

    class Buffer:
        removed = False

        def subscribe(self, queue):
            for line in rows:
                queue.put_nowait(line)

        def get_tail(self, _count):
            return ['history']

        def unsubscribe(self, _queue):
            self.removed = True

    class Socket:
        async def accept(self):
            pass

        async def send_text(self, text):
            messages.append(text)

    buffer = Buffer()
    monkeypatch.setattr(log_streamer, 'get_log_buffer', lambda _name: buffer)
    asyncio.run(log_streamer.stream_log(Socket(), 'demo'))
    assert ''.join(messages).splitlines() == ['history'] + rows
    assert max(len(message.splitlines()) for message in messages) <= 64
    assert len(messages) == 4
    assert buffer.removed


def test_disconnected_socket_releases_log_subscription(monkeypatch):
    class Buffer:
        removed = False

        def subscribe(self, queue):
            queue.put_nowait('line')

        def get_tail(self, _count):
            return []

        def unsubscribe(self, _queue):
            self.removed = True

    class Socket:
        async def accept(self):
            pass

        async def send_text(self, _text):
            raise WebSocketDisconnect()

    buffer = Buffer()
    monkeypatch.setattr(log_streamer, 'get_log_buffer', lambda _name: buffer)
    asyncio.run(log_streamer.stream_log(Socket(), 'demo'))
    assert buffer.removed
