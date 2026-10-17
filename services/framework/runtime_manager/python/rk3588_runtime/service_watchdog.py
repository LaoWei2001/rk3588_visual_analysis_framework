"""Feed the console's systemd watchdog from its actual asyncio event loop."""
import asyncio
import os
import socket
import time


def notify(message: str) -> bool:
    path = os.environ.get("NOTIFY_SOCKET", "")
    if not path.startswith(("/", "@")):
        return False
    if path.startswith("@"):
        path = "\0" + path[1:]
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as channel:
            channel.setblocking(False)
            channel.sendto(message.encode(), path)
        return True
    except OSError:
        return False


async def heartbeat(ready: asyncio.Event, startup_timeout: float = 300) -> None:
    try:
        interval = int(os.environ.get("WATCHDOG_USEC", "0")) / 1_000_000
        owner = int(os.environ.get("WATCHDOG_PID", str(os.getpid())))
    except ValueError:
        return
    if interval <= 0 or owner != os.getpid() or not os.environ.get("NOTIFY_SOCKET"):
        return
    started = time.monotonic()
    while True:
        if not ready.is_set() and time.monotonic() - started > startup_timeout:
            notify("STATUS=Console startup exceeded five-minute grace period")
            return  # systemd owns termination/restart, including blocked startup threads.
        notify("WATCHDOG=1")
        await asyncio.sleep(min(5, interval / 3))
