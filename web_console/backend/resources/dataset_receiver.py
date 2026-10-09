#!/usr/bin/env python3
"""PC-only receiver: Python standard library, HTTP transport, durable save-before-ack."""
from __future__ import annotations
import argparse
import contextlib
import hashlib
import json
import os
import re
import shutil
import signal
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
        ''')
        # Persist the local receiver identity: reopening the same directory must
        # not look like a second computer still blocked by the previous lease.
        with self.db:
            self.db.execute('INSERT OR IGNORE INTO receivers VALUES (?,?)',(self.device,uuid.uuid4().hex))
        self.client = self.db.execute('SELECT client FROM receivers WHERE device=?',(self.device,)).fetchone()[0]

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
            counter = self.db.execute('SELECT next FROM groups WHERE id=?',(key,)).fetchone()
            start = counter[0] if counter else 0
            # A batch size change starts a fresh range after existing directories.
            directory = self.directory/key
            ranges = []
            if directory.exists():
                for child in directory.iterdir():
                    match = re.fullmatch(r'(\d+)_(\d+)',child.name)
                    if match and child.is_dir(): ranges.append(tuple(map(int,match.groups())))
            if ranges:
                low,high = max(ranges)
                start = max(start,low)
                if high-low+1 != batch or start>high: start=max(start,high+1)
                low = low if high-low+1==batch and start<=high else start
            else: low = start
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
                'inventory':self.inventory.snapshot()}

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
            try: self._run()
            finally:
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
