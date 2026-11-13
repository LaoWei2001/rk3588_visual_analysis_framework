#!/usr/bin/env python3
"""PC-only receiver: Python standard library, HTTP transport, durable save-before-ack."""
from __future__ import annotations
import argparse
import contextlib
import errno
import hashlib
import json
import os
import re
import shutil
import signal
import secrets
import sqlite3
import sys
import threading
import time
import uuid
from pathlib import Path
from urllib.error import HTTPError
from urllib.parse import urlencode, urlparse
from urllib.request import Request, build_opener


def normalize_url(url: str) -> str:
    url = url.strip()
    if url and '://' not in url:
        url = 'http://' + url
    parsed = urlparse(url)
    if parsed.scheme not in ('http','https') or not parsed.hostname or parsed.username or parsed.password or parsed.query or parsed.fragment:
        raise ValueError('请输入有效的设备地址，如 192.168.2.41:8080；HTTPS 请填写完整地址')
    # Validate a supplied port before trying to connect.
    _ = parsed.port
    return url.rstrip('/')


class _InventoryStopped(Exception):
    pass


class InventoryScanner:
    """Count local channel folders without involving inference or the save loop."""
    INTERVAL_SECONDS = 5
    def __init__(self, directory: Path, device: str):
        self.root = directory / device
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._snapshot = None
        self._worker = None
        self._entries = 0

    def _tick(self):
        self._entries += 1
        if self._stop.is_set() or (self._entries % 256 == 0 and self._stop.wait(.01)):
            raise _InventoryStopped

    def _count(self, directory: Path) -> int:
        count = 0
        pending = [directory]
        while pending:
            self._tick()
            path = pending.pop()
            try:
                with os.scandir(path) as entries:
                    for entry in entries:
                        self._tick()
                        if entry.is_dir(follow_symlinks=False):
                            pending.append(Path(entry.path))
                        elif entry.name.lower().endswith(('.jpg', '.jpeg')) and entry.is_file(follow_symlinks=False):
                            count += 1
            except FileNotFoundError:
                continue # Deleting a whole batch folder is normal during a scan.
        return count

    def scan(self) -> list[dict]:
        counts = []
        try:
            with os.scandir(self.root) as apps:
                for app in apps:
                    self._tick()
                    if not re.fullmatch(r'[A-Za-z0-9_+\-][A-Za-z0-9_.+\-]{0,119}', app.name) or not app.is_dir(follow_symlinks=False):
                        continue
                    try:
                        with os.scandir(app.path) as channels:
                            for channel in channels:
                                self._tick()
                                match = re.fullmatch(r'ch(\d+)', channel.name)
                                if not match or not 0 <= int(match[1]) < 32 or not channel.is_dir(follow_symlinks=False):
                                    continue
                                counts.append({'app':app.name, 'channel_id':int(match[1]), 'count':self._count(Path(channel.path))})
                    except FileNotFoundError:
                        continue
        except FileNotFoundError:
            pass
        if len(counts) > 1024:
            raise OSError('存图目录超过1024个通道，无法同步完整统计')
        return counts

    def snapshot(self):
        with self._lock:
            return self._snapshot

    def _run(self):
        while not self._stop.is_set():
            try:
                counts = self.scan()
                error = ''
            except _InventoryStopped:
                return
            except OSError as exc:
                counts, error = None, str(exc)[:500]
            with self._lock:
                self._snapshot = {'scan_id':uuid.uuid4().hex, 'counts':counts, 'error':error}
            if self._stop.wait(self.INTERVAL_SECONDS):
                return

    def start(self):
        self._worker = threading.Thread(target=self._run, daemon=True)
        self._worker.start()

    def stop(self):
        self._stop.set()
        # Directory I/O on an unavailable network drive must not delay exit.


class ControlConnection:
    """A warm, bounded control connection, independent of JPEG downloads."""
    def __init__(self, url, client, key):
        import http.client
        parsed = urlparse(url)
        connection = http.client.HTTPSConnection if parsed.scheme == 'https' else http.client.HTTPConnection
        self.connection = connection(parsed.hostname, parsed.port, timeout=2)
        self.path = parsed.path.rstrip('/') + '/api/dataset-receiver/control'
        self.client, self.key = client, key

    def request(self, action, binding_id='', request_id=None, *, task_id=''):
        body = json.dumps({'client': self.client, 'action': action, 'binding_id': binding_id,
                           'request_id': request_id or uuid.uuid4().hex, 'task_id': task_id}).encode()
        try:
            # Uvicorn can close an idle keep-alive connection. Detect that before
            # sending a side-effecting command, never replay an ambiguous POST.
            import select
            if self.connection.sock and select.select([self.connection.sock], [], [], 0)[0]:
                self.connection.close()
            self.connection.request('POST', self.path, body, {'Content-Type': 'application/json',
                                    'X-Receiver-Control': self.key})
            response = self.connection.getresponse()
            data = response.read(65537)
            if len(data) > 65536:
                raise ValueError('快捷键响应过大')
            result = json.loads(data)
            if response.status != 200:
                raise RuntimeError(result.get('detail', f'HTTP {response.status}'))
            return result
        except Exception:
            self.connection.close()
            # Do not replay a key press after network recovery.
            raise

    def close(self):
        self.connection.close()


class GlobalCaptureKeys:
    """Win32 global keys, no keyboard hook, focus requirement or download lock."""
    def __init__(self, url, client, key):
        import queue
        self.url, self.client, self.key = url, client, key
        self.binding = None
        self.targets = []
        self._state_lock = threading.Lock()
        self._target_generation = 0
        self.available = False
        self._stop = threading.Event()
        self._ready = threading.Event()
        self._busy = threading.Lock()
        self._queue = queue.Queue(maxsize=1)
        self._thread_id = None
        self._worker = None
        self._last_result = None
        self._last_seen = 0.0

    def trigger(self, action, pressed_at=None):
        # The listener does no HTTP or file work; reject overlap instead of
        # queueing stale "current frame" captures while a request is in flight.
        with self._state_lock:
            binding = dict(self.binding) if self.binding else None
            targets = list(self.targets)
            fresh = time.monotonic() - self._last_seen <= 5
        if not fresh or (not binding and action != 'select'):
            print('快捷键等待目标：请先创建并运行采集任务；多个任务时按 F7 选择。', flush=True)
            return False
        if action == 'select':
            if not targets:
                print('没有可用的采集任务，请先在网页创建任务并启动视觉程序。', flush=True)
                return False
            index = next((i for i, target in enumerate(targets)
                          if target['task_id'] == (binding or {}).get('task_id')), -1)
            binding = dict(targets[(index + 1) % len(targets)], id='')
        if not self._busy.acquire(blocking=False):
            print('上一条快捷键指令仍在处理，本次未排队。', flush=True)
            return False
        self._queue.put_nowait((action, binding, pressed_at or time.perf_counter()))
        return True

    def _commands(self):
        import queue
        connection = ControlConnection(self.url, self.client, self.key)
        try:
            while not self._stop.is_set():
                try: action, binding, started = self._queue.get(timeout=.5)
                except queue.Empty:
                    # Keep this command lane warm without putting status polling
                    # in front of keys. The status lane handles target discovery.
                    continue
                try:
                    if time.perf_counter() - started > .25:
                        print('按键已过期，本次未抓图，请重新按键。', flush=True)
                        continue
                    request_id = uuid.uuid4().hex
                    if action == 'select':
                        result = connection.request(action, request_id=request_id, task_id=binding['task_id'])
                        with self._state_lock:
                            self._target_generation += 1
                            self.binding = result.get('binding')
                            self.targets = result.get('targets', [])
                            self._last_seen = time.monotonic()
                        print(f"快捷键目标已切换：{binding['app']} / 视频通道 {binding['channel_id']}。", flush=True)
                        continue
                    result = connection.request(action, binding['id'], request_id)
                    elapsed = round((time.perf_counter() - started) * 1000)
                    label = f"{binding['app']} / 视频通道 {binding['channel_id']}"
                    if action == 'toggle':
                        print(f"{label}：自动采集{'已恢复' if result['enabled'] else '已暂停 · F8 手动抓图仍可用'}（{elapsed} 毫秒）。", flush=True)
                    else:
                        self._last_result = (binding['id'], request_id)
                        task = result.get('task', {})
                        print(f'{label}：抓图请求已响应（往返 {elapsed} 毫秒），等待取帧结果。', flush=True)
                        if task.get('manual_state') == 'failed':
                            print('抓图失败：' + task.get('manual_error', ''), flush=True)
                except Exception as exc:
                    print(f'快捷键请求未确认：{exc}；不会自动补发，请检查采集结果。', flush=True)
                finally:
                    self._busy.release()
        finally:
            connection.close()

    def _status(self):
        connection = ControlConnection(self.url, self.client, self.key)
        previous = None
        last_error = ''
        try:
            while not self._stop.is_set():
                try:
                    with self._state_lock:
                        generation = self._target_generation
                    state = connection.request('status')
                    with self._state_lock:
                        # A status response started before F7 must not undo the
                        # newly selected target on the separate command lane.
                        if generation != self._target_generation:
                            continue
                        self.binding = state.get('binding')
                        self.targets = state.get('targets', [])
                        self._last_seen = time.monotonic()
                        binding, targets = self.binding, list(self.targets)
                    last_error = ''
                    current = (binding or {}).get('id'), state.get('enabled'), tuple(t['task_id'] for t in targets)
                    if current != previous:
                        if binding:
                            print(f"电脑快捷键已就绪：{binding['app']} / 视频通道 {binding['channel_id']}；"
                                  f"自动采集{'运行中' if state.get('enabled') else '已暂停 · F8 手动抓图可用'}。F8 抓一张，F9 暂停／恢复。", flush=True)
                        elif not targets:
                            print('快捷键已开启，等待采集任务：请在网页创建任务并启动视觉程序。', flush=True)
                        if len(targets) > 1:
                            print('可选任务：' + '；'.join(f"{t['app']} / 视频通道 {t['channel_id']}" for t in targets)
                                  + '。按 F7 依次选择目标，F8/F9 只操作当前目标。', flush=True)
                        previous = current
                    pending = self._last_result
                    if pending and pending[0] == (self.binding or {}).get('id'):
                        task = connection.request('result', pending[0]).get('task', {})
                        if task.get('manual_id') == pending[1] and task.get('manual_state') in ('captured', 'failed'):
                            if task['manual_state'] == 'captured':
                                print(f"已抓取第 {task.get('manual_frame')} 帧，设备收到请求到取帧 {task.get('manual_wait_ms')} 毫秒；保存完成后会显示文件路径。", flush=True)
                            else:
                                print('抓图失败：' + task.get('manual_error', ''), flush=True)
                            if self._last_result == pending: self._last_result = None
                    elif pending:
                        self._last_result = None
                except Exception as exc:
                    with self._state_lock:
                        if generation == self._target_generation:
                            self.binding = None
                            self.targets = []
                    message = str(exc)
                    if message != last_error:
                        print('电脑快捷键等待连接：' + message, flush=True)
                        last_error = message
                self._stop.wait(.5 if self._last_result else 1)
        finally:
            connection.close()

    def _listen(self):
        import ctypes
        from ctypes import wintypes
        user = ctypes.WinDLL('user32', use_last_error=True)
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        user.RegisterHotKey.argtypes = [wintypes.HWND, ctypes.c_int, wintypes.UINT, wintypes.UINT]
        user.RegisterHotKey.restype = wintypes.BOOL
        user.UnregisterHotKey.argtypes = [wintypes.HWND, ctypes.c_int]
        user.UnregisterHotKey.restype = wintypes.BOOL
        user.GetMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), wintypes.HWND, wintypes.UINT, wintypes.UINT]
        user.GetMessageW.restype = ctypes.c_int
        kernel.GetCurrentThreadId.restype = wintypes.DWORD
        registered = []
        try:
            self._thread_id = kernel.GetCurrentThreadId()
            # MOD_NOREPEAT avoids auto-repeat while the user holds a key down.
            for identifier, key in ((1, 0x77), (2, 0x78), (3, 0x76)): # F8, F9, F7
                if not user.RegisterHotKey(None, identifier, 0x4000, key):
                    raise RuntimeError(f'F{key - 0x6F} 被其他程序占用或无法注册（Windows {ctypes.get_last_error()}），请关闭冲突程序后重启接收端')
                registered.append(identifier)
            self.available = True
            self._ready.set()
            message = wintypes.MSG()
            while not self._stop.is_set():
                result = user.GetMessageW(ctypes.byref(message), None, 0, 0)
                if result == -1: raise ctypes.WinError(ctypes.get_last_error())
                if result == 0: break
                if message.message == 0x0312 and message.wParam in (1, 2, 3): # WM_HOTKEY
                    # A suspended machine must not replay an old WM_HOTKEY.
                    kernel.GetTickCount.restype = wintypes.DWORD
                    if (kernel.GetTickCount() - message.time) & 0xFFFFFFFF > 250:
                        continue
                    self.trigger({1: 'capture', 2: 'toggle', 3: 'select'}[message.wParam])
        except Exception as exc:
            print('全局快捷键未启用：' + str(exc) + '。图片接收仍可继续。', flush=True)
        finally:
            self.available = False
            for identifier in registered: user.UnregisterHotKey(None, identifier)
            self._ready.set()

    def start(self):
        if os.name != 'nt':
            print('当前系统只启用图片接收；全局 F8/F9 快捷键需要 Windows 接收程序。', flush=True)
            return
        self._worker = threading.Thread(target=self._listen, daemon=True)
        self._worker.start()
        self._ready.wait(3)
        if self.available:
            threading.Thread(target=self._commands, daemon=True).start()
            threading.Thread(target=self._status, daemon=True).start()
            print('全局快捷键默认开启：F8 抓一张，F9 暂停／恢复自动采集，F7 切换任务。观看浏览器或播放器时无需切回接收程序。', flush=True)

    def stop(self):
        self._stop.set()
        if os.name == 'nt' and self._thread_id:
            import ctypes
            from ctypes import wintypes
            post = ctypes.WinDLL('user32', use_last_error=True).PostThreadMessageW
            post.argtypes = [wintypes.DWORD, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
            post.restype = wintypes.BOOL
            post(self._thread_id, 0x0012, 0, 0) # WM_QUIT; unregister on owner thread.
        if self._worker: self._worker.join(timeout=1)


class Receiver:
    def __init__(self, url: str, directory: Path):
        self.url = normalize_url(url)
        self.directory = directory.resolve()
        self.directory.mkdir(parents=True,exist_ok=True)
        self.device = hashlib.sha256(self.url.encode()).hexdigest()[:12]
        self.opener = build_opener()
        self._poll_started = False
        self.inventory = InventoryScanner(self.directory, self.device)
        self.db = sqlite3.connect(self.directory/'.dataset_receiver.sqlite')
        self.db.executescript('''
          CREATE TABLE IF NOT EXISTS images (
            id TEXT PRIMARY KEY, path TEXT NOT NULL, metadata TEXT NOT NULL,
            complete INTEGER NOT NULL DEFAULT 0, acknowledged INTEGER NOT NULL DEFAULT 0);
          CREATE TABLE IF NOT EXISTS groups (id TEXT PRIMARY KEY, next INTEGER NOT NULL);
          CREATE TABLE IF NOT EXISTS receivers (device TEXT PRIMARY KEY, client TEXT NOT NULL);
          CREATE TABLE IF NOT EXISTS control_keys (device TEXT PRIMARY KEY, secret TEXT NOT NULL);
          CREATE INDEX IF NOT EXISTS pending_image_paths ON images(path) WHERE complete=0;
        ''')
        # Persist the local receiver identity: reopening the same directory must
        # not look like a second computer still blocked by the previous lease.
        with self.db:
            self.db.execute('INSERT OR IGNORE INTO receivers VALUES (?,?)',(self.device,uuid.uuid4().hex))
        self.client = self.db.execute('SELECT client FROM receivers WHERE device=?',(self.device,)).fetchone()[0]
        with self.db:
            self.db.execute('INSERT OR IGNORE INTO control_keys VALUES (?,?)', (self.device, secrets.token_hex(32)))
        self.control_key = self.db.execute('SELECT secret FROM control_keys WHERE device=?', (self.device,)).fetchone()[0]
        if os.name == 'posix': (self.directory/'.dataset_receiver.sqlite').chmod(0o600)
        self.hotkeys = GlobalCaptureKeys(self.url, self.client, self.control_key)

    def request(self, path: str, body=None, binary=False, *, timeout=20):
        data = json.dumps(body,ensure_ascii=False).encode() if body is not None else None
        request = Request(self.url+'/api/dataset-receiver/'+path, data=data,
                          headers={'Content-Type':'application/json'})
        with self.opener.open(request,timeout=timeout) as response:
            length = response.headers.get('Content-Length')
            if binary and length and int(length)>12*1024**2:
                raise ValueError('图片超过接收大小限制')
            value = response.read(12*1024**2+1)
            if len(value)>12*1024**2:
                raise ValueError('响应超过接收大小限制')
        return value if binary else json.loads(value)

    @staticmethod
    def _has_capture_file(directory: Path) -> bool:
        pending = [directory]
        while pending:
            try:
                with os.scandir(pending.pop()) as entries:
                    for entry in entries:
                        if entry.is_dir(follow_symlinks=False):
                            pending.append(Path(entry.path))
                        elif entry.name.lower().endswith(('.jpg', '.jpeg', '.jpg.part')) and entry.is_file(follow_symlinks=False):
                            return True
            except FileNotFoundError:
                continue
        return False

    def _next_group(self, key: str, batch: int) -> tuple[int, int]:
        counter = self.db.execute('SELECT next FROM groups WHERE id=?', (key,)).fetchone()
        start = counter[0] if counter else 0
        directory = self.directory / key
        ranges, empty, candidates = [], [], []
        if directory.exists():
            for child in directory.iterdir():
                match = re.fullmatch(r'(\d+)_(\d+)', child.name)
                if match and child.is_dir() and not child.is_symlink():
                    candidates.append((tuple(map(int, match.groups())), child))
        # Usually only inspect the newest batch, rather than rescanning every
        # saved image on each download. Empty trailing batches can be skipped.
        for bounds, child in sorted(candidates, reverse=True):
            if self._has_capture_file(child):
                ranges.append(bounds)
                break
            empty.append(child)
        # Failed downloads reserve paths before writing JPEGs. These reservations
        # must survive a retry even if the directory has not been created yet.
        prefix = str(Path(key)) + os.sep
        reserved = self.db.execute('SELECT path FROM images WHERE complete=0 AND path>=? AND path<?',
                                   (prefix, prefix + '\uffff')).fetchall()
        for (path,) in reserved:
            match = re.fullmatch(r'(\d+)_(\d+)', Path(path).parent.name)
            if match:
                ranges.append(tuple(map(int, match.groups())))
        if not reserved and not ranges and not self._has_capture_file(directory):
            # Only the folder cursor restarts. Keep all save/ACK history intact.
            start = 0
            for child in empty:
                try:
                    child.rmdir()
                except OSError as exc:
                    if exc.errno not in (errno.ENOENT, errno.ENOTEMPTY, errno.EEXIST):
                        raise
        if ranges:
            low, high = max(ranges)
            start = max(start, low)
            # A batch size change starts a fresh range after existing directories.
            if high-low+1 != batch or start > high:
                start = max(start, high+1)
            low = low if high-low+1 == batch and start <= high else start
        else:
            low = start
        return start, low

    def receive(self, metadata: dict) -> Path:
        for key in ['sample_id','task_id','revision','app']:
            if not isinstance(metadata.get(key),str) or not re.fullmatch(r'[A-Za-z0-9_.+\-]{1,160}',metadata[key]) or metadata[key].startswith('.'):
                raise ValueError('收到无效的图片编号')
        channel, batch = metadata['channel_id'], metadata['images_per_folder']
        if type(channel) is not int or not 0<=channel<32 or type(batch) is not int or not 1<=batch<=10000:
            raise ValueError('收到无效的通道或分文件夹设置')
        sample_id = metadata['sample_id']
        key = self.device+'/'+metadata['app']+'/ch'+str(channel)
        image_id = self.device+':'+sample_id
        row = self.db.execute('SELECT path,complete FROM images WHERE id=?',(image_id,)).fetchone()
        if row:
            path = self.directory/row[0]
            with self.db:
                self.db.execute('UPDATE images SET metadata=? WHERE id=?',(json.dumps(metadata),image_id))
            if row[1] and path.is_file():
                return path
        else:
            start, low = self._next_group(key, batch)
            directory = self.directory/key
            path = directory/(str(low)+'_'+str(low+batch-1))/(sample_id+'.jpg')
            with self.db:
                self.db.execute('INSERT INTO images (id,path,metadata) VALUES (?,?,?)',
                                (image_id,str(path.relative_to(self.directory)),json.dumps(metadata)))
                self.db.execute('INSERT OR REPLACE INTO groups VALUES (?,?)',(key,start+1))
        query = urlencode({key: metadata[key] for key in ['app','task_id','sample_id']})
        data = self.request('image?'+query,binary=True)
        if len(data)!=metadata['bytes'] or not data.startswith(b'\xff\xd8') or not data.endswith(b'\xff\xd9'):
            raise ValueError('图片下载不完整，稍后重试')
        if shutil.disk_usage(self.directory).free < len(data)+256*1024**2:
            raise OSError('电脑磁盘剩余空间不足256MB，暂停接收')
        path.parent.mkdir(parents=True,exist_ok=True)
        temporary = path.with_suffix('.jpg.part')
        with temporary.open('wb') as output:
            output.write(data); output.flush(); os.fsync(output.fileno())
        os.replace(temporary,path)
        if os.name=='posix':
            fd=os.open(path.parent,os.O_RDONLY)
            try: os.fsync(fd)
            finally: os.close(fd)
        with self.db:
            self.db.execute('UPDATE images SET complete=1 WHERE id=?',(image_id,))
        return path

    def acknowledge_pending(self):
        records=self.db.execute('SELECT id,metadata FROM images WHERE complete=1 AND acknowledged=0 ORDER BY rowid LIMIT 32').fetchall()
        for image_id,raw in records:
            metadata=json.loads(raw)
            if not image_id.startswith(self.device+':'): continue
            body={key:metadata[key] for key in ['app','task_id','revision','sample_id','proof']}
            try: self.request('ack',body)
            except HTTPError as exc:
                if exc.code in (403,409): continue # A changed task can refresh the sample metadata on the next poll.
                raise
            with self.db: self.db.execute('UPDATE images SET acknowledged=1 WHERE id=?',(image_id,))

    def poll_body(self, error='') -> dict:
        return {'client':self.client, 'directory':str(self.directory), 'error':error,
                'inventory':self.inventory.snapshot(),
                'control_hash':hashlib.sha256(self.control_key.encode()).hexdigest() if self.hotkeys.available else ''}

    def cycle(self, error='') -> int:
        self._poll_started = True
        result=self.request('poll',self.poll_body(error))
        self.acknowledge_pending()
        samples=result['samples']
        for metadata in samples:
            path=self.receive(metadata)
            self.acknowledge_pending()
            print('已保存：'+str(path),flush=True)
        return len(samples)

    def disconnect(self):
        if not self._poll_started:
            return
        # Bound the whole cleanup, including DNS resolution. A daemon thread
        # cannot keep the receiver alive if the device or network is unavailable.
        def notify():
            try: self.request('disconnect', {'client':self.client}, timeout=1)
            except Exception: pass # Older devices still expire the heartbeat.
        worker = threading.Thread(target=notify, daemon=True)
        worker.start()
        worker.join(timeout=1)

    def run(self):
        with console_close_notification(self):
            self.inventory.start()
            self.hotkeys.start()
            try: self._run()
            finally:
                self.hotkeys.stop()
                self.inventory.stop()
                self.disconnect()

    def _run(self):
        print('接收目录：'+str(self.directory)+'\n关闭浏览器和 SSH 不影响接收。请保持电脑开机、不休眠；Ctrl+C 停止。',flush=True)
        error=''; delay=1
        while True:
            try:
                if error and shutil.disk_usage(self.directory).free>256*1024**2 and os.access(self.directory,os.W_OK): error=''
                count=self.cycle(error)
                delay=.2 if count else 1
            except HTTPError as exc:
                try: message=json.loads(exc.read()).get('detail','')
                except Exception: message=''
                if exc.code in (401,403):
                    raise RuntimeError('采集接口拒绝连接，请确认设备已更新为免登录采集；跳板或代理可能另有访问限制') from None
                print('等待恢复：'+(message or str(exc)),flush=True); delay=min(15,max(1,delay*2))
            except OSError as exc:
                if getattr(exc,'errno',None) in (13,28) or '磁盘' in str(exc):
                    error=str(exc)[:500]
                    try: self.request('poll',self.poll_body(error))
                    except Exception: pass
                print('等待恢复：'+str(exc),flush=True); delay=min(15,max(1,delay*2))
            except Exception as exc:
                print('等待恢复：'+str(exc),flush=True); delay=min(15,max(1,delay*2))
            time.sleep(delay)


@contextlib.contextmanager
def console_close_notification(receiver: Receiver):
    if os.name != 'nt':
        yield
        return
    import ctypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    handler_type = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_uint)

    @handler_type
    def handle(event):
        if event != 2: # CTRL_CLOSE_EVENT: all attached processes receive it.
            return 0
        receiver.disconnect()
        return 1

    register = kernel.SetConsoleCtrlHandler
    register.argtypes = [handler_type, ctypes.c_int]
    register.restype = ctypes.c_int
    if not register(handle, 1):
        raise ctypes.WinError(ctypes.get_last_error())
    try: yield
    finally: register(handle, 0)


@contextlib.contextmanager
def exclusive(directory: Path, device: str):
    path=directory/('.receiver-'+device+'.lock')
    with path.open('a+b') as handle:
        if os.name=='nt':
            import msvcrt
            handle.seek(0); handle.write(b'0');handle.flush();handle.seek(0)
            try: msvcrt.locking(handle.fileno(),msvcrt.LK_NBLCK,1)
            except OSError: raise RuntimeError('该设备在此目录已有接收程序运行') from None
        else:
            import fcntl
            try: fcntl.flock(handle,fcntl.LOCK_EX|fcntl.LOCK_NB)
            except OSError: raise RuntimeError('该设备在此目录已有接收程序运行') from None
        yield


def main():
    if os.name=='nt':
        signal.signal(signal.SIGBREAK, signal.default_int_handler)
    else:
        signal.signal(signal.SIGTERM, signal.default_int_handler)
    parser=argparse.ArgumentParser(description='将条件采集图片直接保存在电脑，不使用 SSH')
    parser.add_argument('--config',type=Path,default=Path(__file__).with_name('receiver.json'))
    parser.add_argument('--url',help='设备地址，如 192.168.2.41:8080；省略协议时默认 HTTP')
    parser.add_argument('--output',type=Path)
    parser.add_argument('--choose-directory',action='store_true')
    args=parser.parse_args()
    config=json.loads(args.config.read_text(encoding='utf-8')) if args.config.exists() else {}
    address=args.url or config.get('url') or input('请输入设备地址（如 192.168.2.41:8080，HTTPS 请填写完整地址）：').strip()
    config['url']=normalize_url(address)
    config.pop('token',None) # Older configurations remain usable without credentials.
    directory=args.output or (Path(config['directory']) if config.get('directory') and not args.choose_directory else None)
    if directory is None:
        try:
            import tkinter
            from tkinter import filedialog
            window=tkinter.Tk();window.withdraw()
            choice=filedialog.askdirectory(title='选择电脑上的图片保存文件夹')
            window.destroy()
        except Exception:
            choice=input('请输入电脑图片保存目录：').strip()
        if not choice: return
        directory=Path(choice)
    config['directory']=str(directory.resolve())
    args.config.write_text(json.dumps(config,ensure_ascii=False,indent=2),encoding='utf-8')
    if os.name=='posix': args.config.chmod(0o600)
    receiver=Receiver(config['url'],directory)
    try:
        with exclusive(receiver.directory,receiver.device): receiver.run()
    finally: receiver.db.close()


if __name__=='__main__':
    try: main()
    except KeyboardInterrupt: print('\n接收已停止。')
    except Exception as exc: print('启动失败：'+str(exc),file=sys.stderr);sys.exit(1)
