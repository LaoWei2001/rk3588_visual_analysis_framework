"""Local administrators can submit the same manual start operation as the Web UI."""
import asyncio
import grp
import json
import os
from pathlib import Path

from services.runtime_loader import load_runtime

foreground = load_runtime('foreground')


async def listen(path: Path, start):
    async def handle(reader, writer):
        async def send(message):
            writer.write(json.dumps(message, ensure_ascii=False).encode() + b'\n')
            await writer.drain()

        try:
            raw = await asyncio.wait_for(reader.readline(), timeout=5)
            request = json.loads(raw)
            if not isinstance(request, dict) or not all(isinstance(request.get(field), str)
                                                        for field in ('app', 'config')):
                raise ValueError('启动请求必须包含程序名和配置名')
            if request.get('foreground') is True:
                code = await foreground.run(request['app'], request['config'], start, send, lambda: reader.read(1))
                result = {'exit': code}
            else:
                result = await asyncio.to_thread(start, request['app'], request['config'], '本机命令行')
        except Exception as error:
            result = {'ok': False, 'detail': str(error)}
        try:
            await send(result)
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except (BrokenPipeError, ConnectionResetError):
                pass

    server = await asyncio.start_unix_server(handle, path=str(path), limit=8192, start_serving=False)
    try:
        try:
            gid = grp.getgrnam('sudo').gr_gid if os.geteuid() == 0 else os.getegid()
        except KeyError:
            gid = os.getegid()
        os.chown(path, os.geteuid(), gid)
        os.chmod(path, 0o660)
        await server.start_serving()
        return server
    except BaseException:
        server.close()
        await server.wait_closed()
        path.unlink(missing_ok=True)
        raise
