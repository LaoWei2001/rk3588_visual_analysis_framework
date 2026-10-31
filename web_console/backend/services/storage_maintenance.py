"""Storage policy scheduling owned by the application lifecycle."""
import asyncio

from services import storage_manager


async def maintenance_loop() -> None:
    while True:
        await asyncio.sleep(300)
        settings = await asyncio.to_thread(storage_manager.read_settings)
        if settings["auto_cleanup"]:
            await asyncio.to_thread(storage_manager.cleanup_now)
        await asyncio.sleep(3300)
