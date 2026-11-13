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
from typing import Any, Callable

from fastapi import HTTPException
from services.data_dir import APPS_ROOT
from services.dataset_engine import DatasetEngineClient, DatasetEngineRejected, DatasetEngineUnavailable
from services.dataset_validation import validate_rules


class DatasetManager:
    def __init__(self, root: Path = APPS_ROOT, database: Path | None = None,
                 engine: DatasetEngineClient | None = None,
                 process_identity: Callable[[str], str | None] | None = None):
        self.root = root
        self.database = database or root / '.console_dataset.sqlite'
        self.lock = threading.RLock()
        self._ack_secret: str | None = None
        self._control_results: dict[tuple[str, str], dict] = {}
        self.engine = engine if engine is not None else DatasetEngineClient()
        self.process_identity = process_identity or self._process_identity

    @staticmethod
    def _process_identity(app: str) -> str | None:
        from services import process_manager
        return process_manager.get_process_identity(app)

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
          CREATE TABLE IF NOT EXISTS task_runtime (
            task TEXT PRIMARY KEY, process_identity TEXT,
            pause_reason TEXT NOT NULL DEFAULT '');
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
    def task_config(row: dict) -> dict:
        result = json.loads(row['config'])
        # Preserve revisions and pending ACKs of existing tasks while retiring
        # the setting from responses and engine commands.
        result.pop('jpeg_quality', None)
        return result

    @staticmethod
    def command(row: dict) -> dict:
        result = DatasetManager.task_config(row)
        result.update(op='configure', task_id=row['id'], channel_id=row['channel'],
                      revision=row['revision'], saved=row['saved'], enabled=bool(row['enabled']),
                      deadline_ms=int(row['deadline'] * 1000))
        return result

    def sync(self, app: str) -> tuple[dict, str]:
        with self.lock:
            return self._sync(app)

    def pause_app(self, app: str, reason: str = '视觉程序已停止，采集任务已自动暂停；重新启动后请点击“开始采集”') -> None:
        # Durable before stopping/replacing the process, including quick restarts.
        # Never change configs, revisions, deadlines, counters or pending ACKs.
        with self.lock, self.db() as db:
            for row in db.execute('SELECT id FROM tasks WHERE app=? AND enabled=1', (app,)).fetchall():
                db.execute('''INSERT INTO task_runtime(task,pause_reason) VALUES (?,?)
                  ON CONFLICT(task) DO UPDATE SET pause_reason=excluded.pause_reason''', (row['id'], reason))
            db.execute('UPDATE tasks SET enabled=0 WHERE app=? AND enabled=1', (app,))

    def _reconcile_process(self, app: str) -> str | None:
        try:
            self.app_dir(app)
        except HTTPException as exc:
            if exc.status_code != 404:
                raise
            # Deleted app directories may still have durable task records from
            # an older console. They must not retain an enabled state either.
            identity = None
        else:
            identity = self.process_identity(app)
        with self.db() as db:
            rows = db.execute('''SELECT tasks.id,task_runtime.process_identity FROM tasks
              LEFT JOIN task_runtime ON task_runtime.task=tasks.id
              WHERE tasks.app=? AND tasks.enabled=1''', (app,)).fetchall()
            for row in rows:
                previous = row['process_identity']
                if identity is None or (previous is not None and previous != identity):
                    reason = ('视觉程序已停止，采集任务已自动暂停；重新启动后请点击“开始采集”'
                              if identity is None else '视觉程序已重新启动，采集任务已自动暂停；请点击“开始采集”')
                    db.execute('UPDATE tasks SET enabled=0 WHERE id=?', (row['id'],))
                    db.execute('''INSERT INTO task_runtime(task,pause_reason) VALUES (?,?)
                      ON CONFLICT(task) DO UPDATE SET pause_reason=excluded.pause_reason''', (row['id'], reason))
                elif previous is None:
                    # Existing enabled tasks survive a console upgrade/restart
                    # while their vision process is still running.
                    db.execute('''INSERT INTO task_runtime(task,process_identity) VALUES (?,?)
                      ON CONFLICT(task) DO UPDATE SET process_identity=excluded.process_identity''', (row['id'], identity))
        return identity

    def _sync(self, app: str) -> tuple[dict, str]:
        try:
            self._reconcile_process(app)
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
            return {'tasks': [], 'unavailable': exc.status_code == 503}, str(exc.detail)

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
                           'detect_classes': models[0].get('detect_classes', []) if len(models)==1 else [],
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
        with self.lock, self.db() as db:
            pauses = {row['task']: row['pause_reason'] for row in db.execute('SELECT task,pause_reason FROM task_runtime')}
        items = []
        for row in self.rows(app):
            item = live.get(row['id'], {})
            items.append({'id': row['id'], 'channel_id': row['channel'], 'config': self.task_config(row),
                          'enabled': bool(row['enabled']), 'saved': row['saved'], 'deadline': row['deadline'],
                          'current_files': files.get((app, row['channel']), 0) if counts is not None else None,
                          'files_updated_at': receiver.get('inventory_seen', 0), 'files_error': inventory.get('error', ''),
                          'manual_id': item.get('manual_id', ''), 'manual_state': item.get('manual_state', ''),
                          'manual_error': item.get('manual_error', ''), 'manual_wait_ms': item.get('manual_wait_ms', 0),
                          'manual_frame': item.get('manual_frame', -1),
                          'pending': item.get('pending', 0), 'matched': item.get('matched', 0), 'skipped': item.get('skipped', 0),
                          'status': (pauses.get(row['id']) if not row['enabled'] else '') or item.get('status', '配置已保存，等待视觉程序启动' if response.get('unavailable') else error or '等待同步到视觉程序')})
        return {'manual_capture_supported': response.get('manual_capture_supported', False), 'tasks': items, 'channels': self.channels(app), 'receiver': self.receiver_status(receiver), 'engine_error': error}

    def save(self, app: str, channel: int, config: dict) -> dict:
        config = dict(config)
        config.pop('jpeg_quality', None)
        self.app_dir(app)
        with self.lock:
            self._reconcile_process(app)
            return self._save(app, channel, config)

    def _save(self, app: str, channel: int, config: dict) -> dict:
        with self.db() as db:
            old = db.execute('SELECT * FROM tasks WHERE app=? AND channel=?', (app, channel)).fetchone()
            if old and old['enabled']:
                raise HTTPException(409, '请先暂停任务再修改配置')
            row = dict(old) if old else {'id': secrets.token_hex(12), 'app': app, 'channel': channel, 'saved': 0, 'enabled': False, 'deadline': 0}
            try:
                row['config'] = json.dumps(config, ensure_ascii=False, sort_keys=True, allow_nan=False)
            except (TypeError, ValueError) as exc:
                raise HTTPException(409, '采集配置必须使用有限数值和有效的 JSON 字段') from exc
            row['revision'] = hashlib.sha256(row['config'].encode()).hexdigest()[:24]
            if old and json.loads(old['config']).get('duration_hours') != config.get('duration_hours'):
                row['deadline'] = 0
            # Use the live model when available, otherwise validate the saved app
            # configuration. Creation/editing does not depend on engine uptime.
            try:
                self.rpc(app, self.command(row))
            except HTTPException as exc:
                if exc.status_code != 503:
                    raise
                source = next((item for item in self.channels(app) if item['id'] == channel), None)
                if channel < 0 or channel >= 32 or not source or not source['eligible']:
                    raise HTTPException(409, '采集通道不存在、已关闭或未配置单个检测模型') from exc
                labels = source['labels']
                if not labels or len(set(labels)) != len(labels):
                    raise HTTPException(409, '模型类别表无法读取、为空或包含重复名称') from exc
                validate_rules(config.get('rules'), labels, source['detect_classes'], source['threshold'])
            db.execute('''INSERT INTO tasks VALUES (?,?,?,?,?,?,?,?)
              ON CONFLICT(id) DO UPDATE SET config=excluded.config,revision=excluded.revision,deadline=excluded.deadline''',
                       (row['id'], app, channel, row['config'], row['revision'], 0, row['saved'], row['deadline']))
        return self.list(app)

    def capture_once(self, app: str, task_id: str, request_id: str) -> dict:
        # One socket round trip; do not put process reconciliation, status polling or
        # receiver/database updates ahead of a latency-sensitive capture command.
        self.app_dir(app)
        with self.db() as db:
            saved = db.execute('SELECT * FROM tasks WHERE app=? AND id=?', (app, task_id)).fetchone()
            row = dict(saved) if saved else None
        if row is None:
            raise HTTPException(404, '采集任务不存在')
        response, _ = self.rpc(app, {'op': 'capture_once', 'task_id': task_id,
                                    'revision': row['revision'], 'request_id': request_id})
        return next((task for task in response.get('tasks', []) if task['task_id'] == task_id), {})

    def set_enabled(self, app: str, task_id: str, enabled: bool) -> dict:
        with self.lock:
            identity = self._reconcile_process(app)
            return self._set_enabled(app, task_id, enabled, identity)

    def _set_enabled(self, app: str, task_id: str, enabled: bool, identity: str | None) -> dict:
        with self.lock, self.db() as db:
            old = db.execute('SELECT * FROM tasks WHERE app=? AND id=?', (app, task_id)).fetchone()
            if not old:
                raise HTTPException(404, '采集任务不存在')
            if enabled and identity is None:
                raise HTTPException(503, '视觉程序已停止，请先启动视觉程序，再点击“开始采集”')
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
            db.execute('''INSERT INTO task_runtime(task,process_identity,pause_reason) VALUES (?,?,'')
              ON CONFLICT(task) DO UPDATE SET process_identity=excluded.process_identity,pause_reason='' ''',
                       (task_id, identity))
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
            db.execute('DELETE FROM task_runtime WHERE task=?', (task_id,))
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

    def heartbeat(self, client: str, directory: str, error: str, inventory: dict | None = None, control_hash: str = "") -> dict:
        with self.lock:
            previous = self.settings('receiver') or {}
            if not previous.get('disconnected') and previous.get('client') != client and time.time()-previous.get('seen',0) < 30:
                raise HTTPException(409, '已有电脑接收程序在线，请先关闭旧接收程序并等待30秒')
            same_lease = (not previous.get('disconnected') and previous.get('client') == client
                          and time.time() - previous.get('seen', 0) < 30)
            pinned_hash = previous.get('control_hash', '') if same_lease else ''
            if pinned_hash and control_hash and not hmac.compare_digest(pinned_hash, control_hash):
                raise HTTPException(403, '当前接收会话的快捷键凭据不匹配，请先退出原接收程序')
            value = {'client': client, 'directory': directory[:500], 'error': error[:500], 'seen': time.time(),
                     'control_hash': control_hash or pinned_hash, 'control_ready': bool(control_hash)}
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
        binding = self._receiver_binding(state)
        if binding and self.process_identity(binding['app']) is None:
            binding = None
        return {'online': not state.get('disconnected') and time.time()-state.get('seen', 0) < 15, 'directory': state.get('directory', ''),
                'error': state.get('error', ''), 'last_seen': state.get('seen', 0),
                'hotkeys_available': state.get('control_ready', bool(state.get('control_hash'))),
                'control_task': binding}

    def _receiver_binding(self, receiver: dict) -> dict | None:
        binding = self.settings('receiver_control') or {}
        if (not binding or binding.get('client') != receiver.get('client') or
                binding.get('key_hash') != receiver.get('control_hash')):
            return None
        with self.db() as db:
            row = db.execute('SELECT revision FROM tasks WHERE app=? AND id=?',
                             (binding['app'], binding['task_id'])).fetchone()
        if row is None or row['revision'] != binding.get('revision'):
            return None
        return {key: binding[key] for key in ('id', 'app', 'task_id', 'channel_id')}

    def _receiver_targets(self) -> list[dict]:
        rows = self.rows()
        running = {app for app in {row['app'] for row in rows} if self.process_identity(app) is not None}
        return [row for row in rows if row['app'] in running]

    def _select_receiver_target(self, receiver: dict, task_id: str = '') -> dict:
        # A connected PC owns its shortcuts. Single live task: select by default;
        # multiple tasks: retain that PC's selection or let it choose explicitly.
        # Merely selecting a task must never change automatic capture's state.
        with self.lock:
            rows = self._receiver_targets()
            old = self.settings('receiver_control') or {}
            selected = None
            if task_id:
                selected = next((row for row in rows if row['id'] == task_id), None)
                if selected is None:
                    raise HTTPException(409, '目标任务已不可用，请等待任务列表刷新后按 F7 重新选择')
            elif (old.get('client') == receiver.get('client') and
                  old.get('key_hash') == receiver.get('control_hash')):
                selected = next((row for row in rows if row['id'] == old.get('task_id')), None)
            if selected is None and len(rows) == 1:
                selected = rows[0]
            binding = None
            if selected:
                value = {'client': receiver['client'], 'key_hash': receiver['control_hash'],
                         'app': selected['app'], 'task_id': selected['id'],
                         'revision': selected['revision'], 'channel_id': selected['channel']}
                if any(old.get(key) != val for key, val in value.items()):
                    self.settings('receiver_control', dict(value, id=secrets.token_hex(16)))
                binding = self._receiver_binding(receiver)
            return {'binding': binding, 'enabled': bool(selected['enabled']) if selected else False,
                    'targets': [{'task_id': row['id'], 'app': row['app'], 'channel_id': row['channel']}
                                for row in rows]}

    def receiver_control(self, client: str, key: str, action: str, binding_id: str, request_id: str, task_id: str = '') -> dict:
        # Only the active receiver holding the session's pinned random secret
        # can discover/select/control tasks. No web-side enable step is required.
        if not re.fullmatch(r'[a-f0-9]{64}', key):
            raise HTTPException(403, '电脑快捷键凭据无效')
        key_hash = hashlib.sha256(key.encode()).hexdigest()
        receiver = self.settings('receiver') or {}
        if (receiver.get('client') != client or receiver.get('disconnected') or
                time.time() - receiver.get('seen', 0) >= 30 or
                not receiver.get('control_ready', bool(receiver.get('control_hash'))) or
                not hmac.compare_digest(receiver.get('control_hash', ''), key_hash)):
            raise HTTPException(403, '接收程序未连接或快捷键凭据已失效')
        if action in ('status', 'select'):
            if action == 'select' and not task_id:
                raise HTTPException(400, '请选择采集任务')
            return self._select_receiver_target(receiver, task_id if action == 'select' else '')
        binding = self._receiver_binding(receiver)
        if not binding or not hmac.compare_digest(binding['id'], binding_id):
            raise HTTPException(409, '快捷键目标已变化，请等待接收程序刷新后重试；多任务可按 F7 选择')
        if action == 'capture':
            return {'binding': binding, 'task': self.capture_once(binding['app'], binding['task_id'], request_id)}
        if action == 'result':
            response, _ = self.rpc(binding['app'], {'op': 'status'})
            task = next((t for t in response.get('tasks', []) if t['task_id'] == binding['task_id']), {})
            return {'binding': binding, 'task': task}
        if action != 'toggle':
            raise HTTPException(400, '未知快捷键操作')
        with self.lock:
            # A network retry with the same ID must never toggle twice.
            cache_key = (binding_id, request_id)
            if cache_key in self._control_results:
                return self._control_results[cache_key]
            # Revalidate after acquiring the mutation lock, including revocation.
            if self._receiver_binding(self.settings('receiver') or {}) != binding:
                raise HTTPException(409, '快捷键目标已变化')
            rows = self.rows(binding['app'])
            row = next((r for r in rows if r['id'] == binding['task_id']), None)
            if row is None:
                raise HTTPException(404, '采集任务不存在')
            result = self.set_enabled(binding['app'], binding['task_id'], not row['enabled'])
            task = next(t for t in result['tasks'] if t['id'] == binding['task_id'])
            response = {'binding': binding, 'enabled': task['enabled']}
            if len(self._control_results) >= 256:
                self._control_results.pop(next(iter(self._control_results)))
            self._control_results[cache_key] = response
            return response

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
