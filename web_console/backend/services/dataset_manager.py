"""Conditional capture control. SQLite stores settings/counts only, never image bytes."""
from __future__ import annotations

import asyncio
from contextlib import contextmanager
import hashlib
import hmac
import json
import re
import secrets
import sqlite3
import threading
import time
from pathlib import Path
from typing import Any

from fastapi import HTTPException
from services.data_dir import APPS_ROOT
from services.dataset_engine import DatasetEngineClient, DatasetEngineRejected, DatasetEngineUnavailable


class DatasetManager:
    def __init__(self, root: Path = APPS_ROOT, database: Path | None = None,
                 engine: DatasetEngineClient | None = None):
        self.root = root
        self.database = database or root / '.console_dataset.sqlite'
        self.lock = threading.RLock()
        self._ack_secret: str | None = None
        self.engine = engine if engine is not None else DatasetEngineClient()

    @contextmanager
    def db(self):
        self.database.parent.mkdir(parents=True, exist_ok=True)
        connection = sqlite3.connect(self.database, timeout=5)
        self.database.chmod(0o600)
        connection.row_factory = sqlite3.Row
        connection.executescript('''
          CREATE TABLE IF NOT EXISTS tasks (
            id TEXT PRIMARY KEY, app TEXT NOT NULL, channel INTEGER NOT NULL,
            config TEXT NOT NULL, revision TEXT NOT NULL, enabled INTEGER NOT NULL,
            saved INTEGER NOT NULL DEFAULT 0, deadline REAL NOT NULL DEFAULT 0,
            UNIQUE(app,channel));
          CREATE TABLE IF NOT EXISTS acknowledgments (id TEXT PRIMARY KEY, task TEXT NOT NULL);
          CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);
          CREATE TABLE IF NOT EXISTS task_removals (
            id TEXT PRIMARY KEY, app TEXT NOT NULL, command TEXT NOT NULL);
        ''')
        try:
            with connection:
                yield connection
        finally:
            connection.close()

    def app_dir(self, app: str) -> Path:
        if not re.fullmatch(r'[A-Za-z0-9_.+\-]{1,120}', app) or app.startswith('.'):
            raise HTTPException(400, '程序名称无效')
        directory = self.root / app
        if not directory.is_dir():
            raise HTTPException(404, '程序不存在')
        return directory

    def rpc(self, app: str, request: dict) -> tuple[dict, bytes]:
        directory = self.app_dir(app)
        try:
            return self.engine.request(directory, request)
        except DatasetEngineUnavailable as exc:
            raise HTTPException(503, '采集接口未就绪，请启动或重启新版视觉程序') from exc
        except DatasetEngineRejected as exc:
            raise HTTPException(409, str(exc)) from exc

    def rows(self, app: str | None = None) -> list[dict]:
        with self.lock, self.db() as db:
            query = 'SELECT * FROM tasks' + (' WHERE app=?' if app else '') + ' ORDER BY app,channel'
            return [dict(row) for row in db.execute(query, (app,) if app else ())]

    @staticmethod
    def command(row: dict) -> dict:
        result = json.loads(row['config'])
        result.update(op='configure', task_id=row['id'], channel_id=row['channel'],
                      revision=row['revision'], saved=row['saved'], enabled=bool(row['enabled']),
                      deadline_ms=int(row['deadline'] * 1000))
        return result

    def sync(self, app: str) -> tuple[dict, str]:
        with self.lock:
            return self._sync(app)

    def _sync(self, app: str) -> tuple[dict, str]:
        try:
            response, _ = self.rpc(app, {'op': 'status'})
            response = self._sync_removals(app, response)
            existing = {item['task_id']: item for item in response['tasks']}
            for row in self.rows(app):
                old = existing.get(row['id'])
                if not old or old['revision'] != row['revision'] or old.get('enabled') != bool(row['enabled']) or old['saved'] < row['saved']:
                    response, _ = self.rpc(app, self.command(row))
            # ACK may have committed just before the engine socket disconnected.
            # Drain those images on reconnect even if the PC no longer retries the ACK.
            for _ in range(64):
                delivered = None
                with self.db() as db:
                    for task in response['tasks']:
                        image = task.get('next')
                        if image and db.execute('SELECT 1 FROM acknowledgments WHERE id=?',(image['sample_id'],)).fetchone():
                            row = db.execute('SELECT saved FROM tasks WHERE id=?',(task['task_id'],)).fetchone()
                            if row:
                                delivered = (task['task_id'],image['sample_id'],row[0]); break
                if not delivered:
                    break
                task_id,sample_id,saved = delivered
                response, _ = self.rpc(app,{'op':'ack','task_id':task_id,'sample_id':sample_id,'saved':saved})
            return response, ''
        except HTTPException as exc:
            return {'tasks': []}, str(exc.detail)

    def _sync_removals(self, app: str, response: dict) -> dict:
        with self.db() as db:
            removals = [dict(row) for row in db.execute('SELECT * FROM task_removals WHERE app=?', (app,))]
        for removal in removals:
            task_id = removal['id']
            live = next((task for task in response['tasks'] if task['task_id'] == task_id), None)
            if live is not None:
                # A timeout does not prove the old process stopped. Pause it
                # before releasing deleted-task JPEGs through the existing IPC.
                command = json.loads(removal['command'])
                command.update(enabled=False, revision=live['revision'], channel_id=live['channel_id'])
                try:
                    response, _ = self.rpc(app, command)
                    for _ in range(8):
                        live = next((task for task in response['tasks'] if task['task_id'] == task_id), None)
                        image = live.get('next') if live else None
                        if not image:
                            break
                        response, _ = self.rpc(app, {'op': 'ack', 'task_id': task_id,
                                                    'sample_id': image['sample_id'], 'saved': live['saved']})
                    response, _ = self.rpc(app, {'op': 'remove', 'task_id': task_id})
                except HTTPException as exc:
                    if exc.status_code != 409:
                        raise
                    # An encoder may still be finishing. Retry next maintenance
                    # cycle; keep this deletion durable and do not resume it.
                    continue
            with self.db() as db:
                db.execute('DELETE FROM task_removals WHERE id=?', (task_id,))
        return response

    def channels(self, app: str) -> list[dict]:
        directory = self.app_dir(app)
        run_config = directory/'run.config'
        filename = run_config.read_text().strip() if run_config.exists() else 'config.json'
        path = (directory/'assets'/filename).resolve()
        if path.parent != (directory/'assets').resolve():
            raise HTTPException(400, '运行配置路径无效')
        try:
            config = json.loads(path.read_text())
        except (OSError, ValueError):
            return []
        result = []
        for index, channel in enumerate(config.get('channels', [])):
            if not channel.get('enable', True):
                continue
            models = [m for m in channel.get('models', []) if m.get('enable', True)]
            labels = []
            if len(models) == 1:
                label_path = Path(models[0].get('label_path', ''))
                if not label_path.is_absolute():
                    label_path = directory/label_path
                # Do not expose arbitrary files referenced by a malformed configuration.
                if label_path.resolve().is_relative_to((directory/'assets').resolve()) and label_path.is_file():
                    labels = [s for s in label_path.read_text(errors='replace').splitlines() if s]
            result.append({'id': channel.get('id', index), 'logic': channel.get('logic', ''),
                           'labels': labels, 'threshold': models[0].get('obj_thresh', .3) if len(models)==1 else .3,
                           'eligible': len(models)==1 and models[0].get('model_type') in ['yolov8_det','yolov5','yolov5_seg'],
                           'rois': [r.get('name','') for r in channel.get('roi_zones', [])]})
        return result

    def list(self, app: str) -> dict:
        response, error = self.sync(app)
        live = {t['task_id']: t for t in response['tasks']}
        receiver = self.settings('receiver') or {}
        inventory = receiver.get('inventory') or {}
        counts = inventory.get('counts')
        files = {(item['app'], item['channel_id']): item['count'] for item in counts or []}
        items = []
        for row in self.rows(app):
            item = live.get(row['id'], {})
            items.append({'id': row['id'], 'channel_id': row['channel'], 'config': json.loads(row['config']),
                          'enabled': bool(row['enabled']), 'saved': row['saved'], 'deadline': row['deadline'],
                          'current_files': files.get((app, row['channel']), 0) if counts is not None else None,
                          'files_updated_at': receiver.get('inventory_seen', 0), 'files_error': inventory.get('error', ''),
                          'pending': item.get('pending', 0), 'matched': item.get('matched', 0), 'skipped': item.get('skipped', 0),
                          'status': item.get('status', error or '等待视觉程序启动')})
        return {'tasks': items, 'channels': self.channels(app), 'receiver': self.receiver_status(receiver), 'engine_error': error}

    def save(self, app: str, channel: int, config: dict) -> dict:
        self.app_dir(app)
        with self.lock, self.db() as db:
            old = db.execute('SELECT * FROM tasks WHERE app=? AND channel=?', (app, channel)).fetchone()
            if old and old['enabled']:
                raise HTTPException(409, '请先暂停任务再修改配置')
            row = dict(old) if old else {'id': secrets.token_hex(12), 'app': app, 'channel': channel, 'saved': 0, 'enabled': False, 'deadline': 0}
            row['config'] = json.dumps(config, ensure_ascii=False, sort_keys=True, allow_nan=False)
            row['revision'] = hashlib.sha256(row['config'].encode()).hexdigest()[:24]
            if old and json.loads(old['config']).get('duration_hours') != config.get('duration_hours'):
                row['deadline'] = 0
            # Validate against the running model, including labels/filters/confidence floor.
            self.rpc(app, self.command(row))
            db.execute('''INSERT INTO tasks VALUES (?,?,?,?,?,?,?,?)
              ON CONFLICT(id) DO UPDATE SET config=excluded.config,revision=excluded.revision,deadline=excluded.deadline''',
                       (row['id'], app, channel, row['config'], row['revision'], 0, row['saved'], row['deadline']))
        return self.list(app)

    def set_enabled(self, app: str, task_id: str, enabled: bool) -> dict:
        with self.lock, self.db() as db:
            old = db.execute('SELECT * FROM tasks WHERE app=? AND id=?', (app, task_id)).fetchone()
            if not old:
                raise HTTPException(404, '采集任务不存在')
            row = dict(old)
            if enabled and not row['enabled']:
                config = json.loads(row['config'])
                duration = config.get('duration_hours', 0)
                # Resume existing deadlines; a finished task needs a fresh task/reset.
                if duration and not row['deadline']:
                    row['deadline'] = time.time() + duration * 3600
                if row['deadline'] and row['deadline'] <= time.time():
                    raise HTTPException(409, '任务时长已结束，请删除后重新创建')
                if config['max_samples'] and row['saved'] >= config['max_samples']:
                    raise HTTPException(409, '任务已达到图片上限，请提高上限或重新创建')
            row['enabled'] = enabled
            try:
                self.rpc(app, self.command(row))
            except HTTPException as exc:
                if enabled or exc.status_code != 503:
                    raise
                # A stopped/disconnected engine must not prevent cancelling auto-resume.
                # The reconciler applies this pause as soon as its socket returns.
            db.execute('UPDATE tasks SET enabled=?,deadline=? WHERE id=?', (int(enabled), row['deadline'], task_id))
        return self.list(app)

    def remove(self, app: str, task_id: str) -> dict:
        with self.lock, self.db() as db:
            row = db.execute('SELECT * FROM tasks WHERE app=? AND id=?', (app, task_id)).fetchone()
            if not row:
                raise HTTPException(404, '采集任务不存在')
            offline = False
            if row['enabled']:
                try:
                    self.rpc(app, {'op': 'status'})
                except HTTPException as exc:
                    if exc.status_code != 503:
                        raise
                    offline = True
                if not offline:
                    raise HTTPException(409, '请先暂停任务')
            else:
                try:
                    self.rpc(app, {'op': 'remove', 'task_id': task_id})
                except HTTPException as exc:
                    if exc.status_code != 503:
                        raise
                    offline = True
            if offline:
                command = self.command(dict(row))
                command['enabled'] = False
                db.execute('INSERT OR REPLACE INTO task_removals VALUES (?,?,?)',
                           (task_id, app, json.dumps(command, ensure_ascii=False)))
            db.execute('DELETE FROM tasks WHERE id=?', (task_id,))
            db.execute('DELETE FROM acknowledgments WHERE task=?', (task_id,))
        return self.list(app)

    def settings(self, key: str, value: Any = None):
        with self.lock, self.db() as db:
            if value is not None:
                db.execute('INSERT OR REPLACE INTO settings VALUES (?,?)', (key, json.dumps(value)))
                return value
            row = db.execute('SELECT value FROM settings WHERE key=?', (key,)).fetchone()
            return json.loads(row[0]) if row else None

    def _ack_key(self) -> str:
        # Internal signature key for save confirmations, never a receiver login
        # credential. Cache it so each JPEG/ACK avoids another SQLite connection.
        with self.lock:
            if self._ack_secret is not None:
                return self._ack_secret
            with self.db() as db:
                values = {row['key']: json.loads(row['value']) for row in
                          db.execute("SELECT key,value FROM settings WHERE key IN ('ack_key','token')")}
                # Preserve signatures for images queued before upgrading.
                key = values.get('ack_key') or values.get('token') or secrets.token_hex(32)
                db.execute('INSERT OR REPLACE INTO settings VALUES (?,?)', ('ack_key', json.dumps(key)))
            self._ack_secret = key
            return self._ack_secret

    def heartbeat(self, client: str, directory: str, error: str, inventory: dict | None = None) -> dict:
        with self.lock:
            previous = self.settings('receiver') or {}
            if not previous.get('disconnected') and previous.get('client') != client and time.time()-previous.get('seen',0) < 30:
                raise HTTPException(409, '已有电脑接收程序在线，请先关闭旧接收程序并等待30秒')
            value = {'client': client, 'directory': directory[:500], 'error': error[:500], 'seen': time.time()}
            same_snapshot = (previous.get('client') == client and previous.get('directory') == value['directory']
                             and previous.get('inventory') == inventory)
            value.update(inventory=inventory, inventory_seen=(previous.get('inventory_seen', 0) if same_snapshot else time.time())
                         if inventory is not None else 0)
            return self.settings('receiver', value)

    def disconnect(self, client: str) -> bool:
        with self.lock:
            state = self.settings('receiver') or {}
            if state.get('client') != client:
                return False
            if not state.get('disconnected'):
                state.update(disconnected=True, error='')
                self.settings('receiver', state)
            return True

    def receiver_status(self, state: dict | None = None) -> dict:
        if state is None:
            state = self.settings('receiver') or {}
        return {'online': not state.get('disconnected') and time.time()-state.get('seen', 0) < 15, 'directory': state.get('directory', ''),
                'error': state.get('error', ''), 'last_seen': state.get('seen', 0)}

    def proof(self, task_id: str, revision: str, sample_id: str) -> str:
        key = self._ack_secret or self._ack_key()
        return hmac.new(key.encode(), json.dumps([task_id,revision,sample_id]).encode(), hashlib.sha256).hexdigest()

    def pending(self) -> list[dict]:
        items = []
        rows = self.rows()
        for app in sorted({row['app'] for row in rows}):
            response, _ = self.sync(app)
            configs = {row['id']: row for row in rows if row['app']==app}
            for task in response['tasks']:
                row = configs.get(task['task_id'])
                if row and task.get('next'):
                    sample = dict(task['next'])
                    sample.update(app=app, task_id=row['id'], revision=row['revision'], channel_id=row['channel'],
                                  images_per_folder=json.loads(row['config'])['images_per_folder'])
                    sample['proof'] = self.proof(row['id'], row['revision'], sample['sample_id'])
                    items.append(sample)
        return items

    def image(self, app: str, task_id: str, sample_id: str) -> bytes:
        if not any(row['id'] == task_id for row in self.rows(app)):
            raise HTTPException(404, '采集任务不存在')
        _, data = self.rpc(app, {'op': 'image', 'task_id': task_id, 'sample_id': sample_id})
        return data

    def acknowledge(self, app: str, task_id: str, revision: str, sample_id: str, proof: str) -> int:
        if not hmac.compare_digest(self.proof(task_id,revision,sample_id), proof):
            raise HTTPException(403, '图片保存确认无效')
        with self.lock, self.db() as db:
            row = db.execute('SELECT * FROM tasks WHERE app=? AND id=?', (app,task_id)).fetchone()
            if not row or row['revision'] != revision:
                raise HTTPException(409, '采集任务已变化')
            inserted = db.execute('INSERT OR IGNORE INTO acknowledgments VALUES (?,?)',(sample_id,task_id)).rowcount
            if inserted:
                db.execute('UPDATE tasks SET saved=saved+1 WHERE id=?',(task_id,))
            saved = db.execute('SELECT saved FROM tasks WHERE id=?',(task_id,)).fetchone()[0]
        # Commit actual PC progress first: an engine/backend restart cannot lose or double count it.
        try:
            self.rpc(app, {'op':'ack','task_id':task_id,'sample_id':sample_id,'saved':saved})
        except HTTPException:
            pass
        return saved

    async def maintenance(self):
        while True:
            def reconcile():
                with self.lock, self.db() as db:
                    apps = [row[0] for row in db.execute('SELECT app FROM tasks UNION SELECT app FROM task_removals ORDER BY app')]
                for app in apps:
                    self.sync(app)
            await asyncio.to_thread(reconcile)
            await asyncio.sleep(2)


dataset_manager = DatasetManager()
