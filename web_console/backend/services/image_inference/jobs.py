"""Durable, bounded batches. Each batch owns one native multi-model session."""
import copy
from collections import Counter
from io import BytesIO
from dataclasses import dataclass, field
import json
from pathlib import Path
import re
import shutil
import signal
import subprocess
import threading
import time
from typing import Optional
import uuid
import zipfile

from .engine import EngineCatalog, IMAGE_EXTENSIONS, JobError, engine_environment, npu_busy
from .storage import require_space, storage_info
from .sources import scan_device_images, snapshot_device_image
from .labels import detection_model, yolo_labels

TERMINAL = frozenset(('completed', 'completed_with_errors', 'failed', 'cancelled'))
JOB_ID = re.compile(r'[0-9a-f]{32}\Z')
ARTIFACT = re.compile(r'[0-9]{6,}/(?:original\.jpg|annotated\.jpg|model_[0-9]+\.jpg)\Z')
RESULT_CACHE_LIMIT = 5


def read_json(path: Path) -> dict:
    with path.open(encoding='utf-8') as source:
        value = json.load(source)
    if not isinstance(value, dict):
        raise ValueError('Expected a JSON object')
    return value


def write_json(path: Path, value: dict):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, ensure_ascii=False), encoding='utf-8')
    temporary.replace(path)


@dataclass
class Job:
    directory: Path
    data: dict
    cancel: threading.Event = field(default_factory=threading.Event)
    process: Optional[subprocess.Popen] = None
    thread: Optional[threading.Thread] = None


class ImageJobs:
    def __init__(self, root: Path, catalog: EngineCatalog):
        self.root, self.catalog = root.resolve(), catalog
        self._jobs = {}
        self._lock = threading.RLock()
        self._cleanup_lock = threading.Lock()
        self._loaded = False
        self._closing = False

    def recover(self):
        with self._lock:
            if self._loaded:
                return
            self.root.mkdir(parents=True, exist_ok=True, mode=0o700)
            for path in self.root.iterdir():
                if not JOB_ID.fullmatch(path.name) or not path.is_dir() or path.is_symlink():
                    continue
                try:
                    data = read_json(path / 'status.json')
                    if data.get('id') != path.name:
                        continue
                    job = Job(path, data)
                    if data.get('status') not in TERMINAL:
                        self._progress(job)
                        self._finish(job, 'failed', '控制台重启，任务已中断；已完成的结果可以继续查看。')
                    self._jobs[path.name] = job
                except (OSError, ValueError):
                    continue
            self._loaded = True
            self._prune()

    def _persist(self, job: Job):
        write_json(job.directory / 'status.json', job.data)

    def _get(self, job_id: str) -> Job:
        self.recover()
        job = self._jobs.get(job_id) if JOB_ID.fullmatch(job_id) else None
        if job is None:
            raise JobError(404, '图片任务不存在或已清理')
        return job

    def _prune(self):
        # Bound disk use while retaining recent results across page reloads.
        if not self._cleanup_lock.acquire(blocking=False):
            return
        try:
            finished = sorted((j for j in self._jobs.values() if j.data['status'] in TERMINAL),
                              key=lambda j: j.data['created_at'], reverse=True)
            for job in finished[RESULT_CACHE_LIMIT:]:
                shutil.rmtree(job.directory, ignore_errors=True)
                self._jobs.pop(job.data['id'], None)
        finally:
            self._cleanup_lock.release()

    def reserve(self, settings: dict, files: list, source: Optional[dict] = None, sizes: Optional[list] = None) -> str:
        public = self.catalog.validate(settings)
        native = copy.deepcopy(public)
        if not files:
            raise JobError(422, '请至少选择一张图片')
        if sizes is not None and (len(sizes) != len(files) or any(type(size) is not int or size <= 0 for size in sizes)):
            raise JobError(422, '图片不能为空，且必须提供正确的文件大小')
        names = []
        for name in files:
            name = Path((name or '').replace('\\', '/')).name
            if not name or len(name) > 240 or Path(name).suffix.lower() not in IMAGE_EXTENSIONS:
                raise JobError(422, '支持 JPG、PNG、BMP、WebP 图片；文件名最多 240 字符')
            names.append(name)
        with self._lock:
            self.recover()
            if self._closing:
                raise JobError(503, '控制台正在关闭')
            if any(j.data['status'] not in TERMINAL for j in self._jobs.values()) or npu_busy():
                raise JobError(409, 'NPU 正被其他任务占用，请先在程序管理中停止视频程序，或等待当前图片任务结束。')
            require_space(self.root, sum(sizes) if sizes else 0)
            job_id = uuid.uuid4().hex
            directory = self.root / job_id
            directory.mkdir(mode=0o700)
            (directory / 'input').mkdir()
            items = [dict(index=i, name=name, status='pending') for i, name in enumerate(names)]
            if sizes is not None:
                for item, size in zip(items, sizes):
                    item['size'] = size
            data = dict(id=job_id, status='uploading', created_at=time.time(),
                        settings=public, source=source or {'kind': 'upload'}, items=items,
                        error='', completed=0, total=len(items))
            job = Job(directory, data)
            self._jobs[job_id] = job
            try:
                native.update(output_dir=str(directory / 'output'), images=[
                    dict(path=str(directory / 'input' / (f'{i:06d}' + Path(name).suffix.lower())), name=name)
                    for i, name in enumerate(names)])
                write_json(directory / 'job.json', native)
                self._persist(job)
                require_space(self.root, sum(sizes) if sizes else 0)
            except BaseException:
                self._jobs.pop(job_id, None)
                shutil.rmtree(directory, ignore_errors=True)
                raise
            return job_id

    def create_from_device(self, settings: dict, path: str, recursive: bool = False) -> dict:
        selection = scan_device_images(path, recursive)
        job_id = self.reserve(settings, [image.name for image in selection.images], selection.source(),
                              [image.size for image in selection.images])
        submitted = False
        try:
            with self._lock:
                job = self._get(job_id)
                native = read_json(job.directory / 'job.json')
                for index, image in enumerate(selection.images):
                    job.data['items'][index].update(name=image.name, source_path=str(image.path))
                    native['images'][index]['name'] = image.name
                write_json(job.directory / 'job.json', native)
                self._persist(job)
            def checkpoint():
                self.input_path(job_id, 0)
            for index, image in enumerate(selection.images):
                target = self.input_path(job_id, index)
                snapshot_device_image(image, target, checkpoint)
            result = self.submit(job_id)
            submitted = True
            return result
        finally:
            if not submitted:
                self.discard_upload(job_id)

    def input_path(self, job_id: str, index: int) -> Path:
        with self._lock:
            job = self._get(job_id)
            if job.data['status'] != 'uploading':
                raise JobError(409, '任务已不接受图片输入')
            if not 0 <= index < job.data['total']:
                raise JobError(404, '图片不存在')
            name = job.data['items'][index]['name']
            return job.directory / 'input' / (f'{index:06d}' + Path(name).suffix.lower())

    def storage(self) -> dict:
        return storage_info(self.root)

    def upload_size(self, job_id: str, index: int) -> int:
        with self._lock:
            self.input_path(job_id, index)
            return self._get(job_id).data['items'][index]['size']

    def write_input(self, job_id: str, index: int, output, chunk: bytes):
        with self._lock:
            self.input_path(job_id, index)  # Recheck cancellation before every write.
            require_space(self.root, len(chunk))
            output.write(chunk)
            output.flush()
            require_space(self.root)

    def discard_upload(self, job_id: str):
        with self._lock:
            job = self._get(job_id)
            if job.data['status'] == 'uploading':
                shutil.rmtree(job.directory, ignore_errors=True)
                self._jobs.pop(job_id, None)

    def submit(self, job_id: str) -> dict:
        with self._lock:
            job = self._get(job_id)
            if job.data['status'] != 'uploading' or self._closing:
                raise JobError(409, '任务不能重复提交')
            paths = [self.input_path(job_id, i) for i in range(job.data['total'])]
            if any(not p.is_file() or p.stat().st_size <= 0
                   or ('size' in item and p.stat().st_size != item['size'])
                   for p, item in zip(paths, job.data['items'])):
                raise JobError(422, '图片为空或上传不完整')
            require_space(self.root)
            job.data['status'] = 'loading'
            self._persist(job)
            job.thread = threading.Thread(target=self._run, args=(job,), name='image-inference', daemon=True)
            job.thread.start()
            return copy.deepcopy(job.data)

    def _progress(self, job: Job):
        try:
            progress = read_json(job.directory / 'output' / 'progress.json')
        except (OSError, ValueError):
            return
        items = progress.get('items')
        if isinstance(items, list) and len(items) == job.data['total']:
            # Keep source provenance alongside progress produced by the native CLI.
            job.data['items'] = [{**previous, **current} for previous, current in zip(job.data['items'], items)]
            job.data['completed'] = sum(i.get('status') in ('success', 'partial', 'failed', 'cancelled') for i in items)
        if progress.get('error'):
            job.data['error'] = str(progress['error'])
        # A native final status is not public until the process releases the NPU.
        if job.data['status'] != 'cancelling' and progress.get('status') in ('loading', 'running'):
            job.data['status'] = progress['status']

    def _finish(self, job: Job, status: str, error: str = ''):
        job.data.update(status=status, finished_at=time.time())
        if error:
            job.data['error'] = error
        for item in job.data['items']:
            if item.get('status') in ('pending', 'running'):
                item['status'] = 'cancelled' if status == 'cancelled' else 'failed'
                item['error'] = error or '任务未完成'
        job.data['completed'] = len(job.data['items'])
        self._persist(job)

    def _run(self, job: Job):
        forced_error = ''
        process = None
        interrupted, last_size_check = None, 0.0
        try:
            with (job.directory / 'engine.log').open('wb') as log:
                process = subprocess.Popen([str(self.catalog.binary), '--infer-images', str(job.directory / 'job.json')],
                    cwd=job.directory, env=engine_environment(self.catalog.runtime), stdout=log, stderr=subprocess.STDOUT,
                    start_new_session=True)
                with self._lock:
                    job.process = process
                while process.poll() is None:
                    now = time.monotonic()
                    if now - last_size_check >= 1:
                        last_size_check = now
                        try:
                            require_space(self.root)
                        except JobError as exc:
                            forced_error = str(exc)
                            job.cancel.set()
                    if job.cancel.is_set():
                        if interrupted is None:
                            interrupted = now
                            process.send_signal(signal.SIGINT)
                        elif now - interrupted > 10:
                            process.kill()
                    with self._lock:
                        self._progress(job)
                    time.sleep(.2)
                process.wait()
            with self._lock:
                self._progress(job)
                try:
                    native = read_json(job.directory / 'output' / 'progress.json')
                except (OSError, ValueError):
                    native = {}
                if forced_error:
                    self._finish(job, 'failed', forced_error)
                elif job.cancel.is_set():
                    self._finish(job, 'cancelled')
                elif native.get('status') in ('completed', 'completed_with_errors') and process.returncode in (0, 1):
                    self._finish(job, native['status'])
                else:
                    error = native.get('error') or f'推理进程异常退出（{process.returncode}）'
                    self._finish(job, 'failed', str(error))
        except Exception as exc:
            if process and process.poll() is None:
                process.kill()
                process.wait()
            with self._lock:
                self._finish(job, 'failed', str(exc))
        finally:
            with self._lock:
                job.process = None
                self._prune()

    def current(self) -> Optional[dict]:
        with self._lock:
            self.recover()
            job = max(self._jobs.values(), default=None,
                      key=lambda j: (j.data['status'] not in TERMINAL, j.data['created_at']))
            return copy.deepcopy(job.data) if job else None

    def clear_completed(self) -> dict:
        # Snapshot finished batches; a batch running at this point is preserved.
        with self._cleanup_lock:
            with self._lock:
                self.recover()
                completed = [(job_id, job) for job_id, job in self._jobs.items()
                             if job.data['status'] in TERMINAL]
            # Workers need the state lock to finish, so join outside it.
            for _, job in completed:
                if job.thread:
                    job.thread.join(timeout=5)
                    if job.thread.is_alive():
                        raise JobError(409, '任务正在结束，请稍后清除记录')
            with self._lock:
                cleared = 0
                for job_id, job in completed:
                    if job.directory.exists():
                        shutil.rmtree(job.directory)
                    self._jobs.pop(job_id, None)
                    cleared += 1
                return {'cleared': cleared, 'current': self.current()}

    def cancel(self, job_id: str) -> dict:
        with self._lock:
            job = self._get(job_id)
            if job.data['status'] == 'uploading':
                job.cancel.set()
                self._finish(job, 'cancelled', '图片准备已取消')
            elif job.data['status'] not in TERMINAL:
                job.cancel.set()
                job.data['status'] = 'cancelling'
                self._persist(job)
            return copy.deepcopy(job.data)

    @staticmethod
    def _output_file(job: Job, path: str) -> Path:
        root = (job.directory / 'output').resolve()
        target = (root / path).resolve()
        if root not in target.parents or not target.is_file():
            raise JobError(404, '结果文件不存在')
        return target

    def result(self, job_id: str, index: int) -> dict:
        # Structured data drives the preview; it is not a downloadable file.
        with self._lock:
            job = self._get(job_id)
            if not 0 <= index < job.data['total']:
                raise JobError(404, '图片结果不存在')
            return read_json(self._output_file(job, f'{index:06d}/result.json'))

    def artifact(self, job_id: str, path: str) -> Path:
        with self._lock:
            job = self._get(job_id)
            if not ARTIFACT.fullmatch(path):
                raise JobError(404, '结果文件不存在')
            return self._output_file(job, path)

    def labels_archive(self, job_id: str, indices: list) -> bytes:
        # Read under the state lock so cleanup cannot remove a selected result.
        # The response owns its bytes; no shared ZIP cache or download files remain.
        with self._lock:
            job = self._get(job_id)
            if job.data['status'] not in TERMINAL:
                raise JobError(409, '请等待任务结束后导出')
            model = detection_model(job.data['settings'])
            if (not indices or len(set(indices)) != len(indices)
                    or any(type(i) is not int or not 0 <= i < job.data['total'] for i in indices)):
                raise JobError(422, '请选择有效且不重复的图片')
            entries = []
            for index in sorted(indices):
                item = job.data['items'][index]
                if item['status'] != 'success' or not item.get('result'):
                    raise JobError(422, f"{item['name']} 尚无成功的检测结果")
                try:
                    result = self.result(job_id, index)
                except ValueError as exc:
                    raise JobError(422, f"{item['name']} 的结果数据损坏") from exc
                entries.append((index, yolo_labels(result, model)))
            # Keep original stems; duplicate names use their image number as a folder.
            # Count the entire batch so subset and full exports use identical paths.
            names = [re.sub(r'[<>:"/\\|?*\x00-\x1f]', '_',
                            Path(item['name'].replace('\\', '/')).stem).rstrip(' .') or 'image'
                     for item in job.data['items']]
            counts = Counter(name.casefold() for name in names)
        output = BytesIO()
        with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
            for index, labels in entries:
                name = names[index]
                path = f'{index + 1:06d}/{name}.txt' if counts[name.casefold()] > 1 else f'{name}.txt'
                archive.writestr(path, labels.encode('utf-8'))
        return output.getvalue()

    def shutdown(self):
        with self._lock:
            self._closing = True
            jobs = list(self._jobs.values())
            for job in jobs:
                if job.data['status'] not in TERMINAL:
                    job.cancel.set()
                    if not job.thread:
                        self._finish(job, 'cancelled', '控制台关闭，图片准备已中断')
        for job in jobs:
            if job.thread:
                job.thread.join(timeout=15)
