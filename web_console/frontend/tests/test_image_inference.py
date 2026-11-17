"""Browser checks for package model selection and simulated inference results.

Build the frontend first; run with PYTHONPATH=web_console/backend.
"""
import base64
import json
import zipfile
import os
from pathlib import Path
import re
import socket
import subprocess
import threading
import time
import uuid

import pytest
import uvicorn
from fastapi import FastAPI, Request
from fastapi.responses import FileResponse, Response
from fastapi.staticfiles import StaticFiles
from routers import image_inference as routes
from routers import assets as package_assets
from services.image_inference.jobs import ImageJobs, Job, write_json

playwright = pytest.importorskip('playwright.sync_api')
FRONTEND = Path(__file__).resolve().parents[1]
PNG = base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Wl2ZQAAAABJRU5ErkJggg==')
BASE = '/api/image-inference'


@pytest.fixture
def console(tmp_path, monkeypatch):
    if not (FRONTEND / 'dist/index.html').exists():
        pytest.skip('Build frontend first')
    app, state = FastAPI(), {}
    # Any endpoint not simulated below must stay isolated from board records.
    monkeypatch.setattr(routes, 'image_jobs', ImageJobs(tmp_path / 'jobs', routes.catalog))

    packages = [
        dict(name='demo', path='/packages/demo', models=['assets/first.rknn', 'assets/second.rknn', 'assets/broken.rknn', 'assets/valid.rknn'], labels=['assets/labels.txt']),
        dict(name='other', path='/packages/other', models=['assets/first.rknn'], labels=['assets/other.txt']),
        dict(name='canvas-demo', path='/canvas', models=['assets/canvas.rknn'], labels=['assets/canvas.txt']),
        dict(name='empty', path='/packages/empty', models=[], labels=[]),
    ]
    state['package_root'] = tmp_path / 'packages'
    monkeypatch.setattr(package_assets, 'APPS_ROOT', state['package_root'])
    for package in packages:
        folder = state['package_root'] / package['name']
        (folder / 'assets').mkdir(parents=True)
        for name in package['models'] + package['labels']:
            (folder / name).write_bytes(b'original')

    @app.get('/api/apps')
    def apps():
        state['app_requests'] = state.get('app_requests', 0) + 1
        result = []
        for package in packages:
            uploaded = [f'assets/{path.name}' for path in sorted((state['package_root'] / package['name'] / 'assets').glob('*.rknn'))
                        if f'assets/{path.name}' not in package['models']]
            result.append(dict(package, models=package['models'] + uploaded))
        return state.get('packages', result)

    @app.post(BASE + '/canvas-preset')
    async def preset(request: Request):
        data = await request.json()
        state['preset'] = data
        for model in data['settings']['models']:
            for field in ['model_path', 'label_path']:
                if model.get(field): model[field] = '/canvas/' + model[field]
        return data['settings']

    @app.get(BASE + '/capabilities')
    def capabilities(): return dict(models=[dict(type=t, result_kind='detection', requires_labels=True) for t in ['yolov8_det', 'yolov5']],
        max_models=8, free_bytes=10 * 1024**3, upload_available_bytes=10 * 1024**3 - 104857601, min_free_bytes=104857600, busy=False)

    @app.get(BASE + '/models/info')
    def inspect_model(path: str, model_type: str):
        state.setdefault('model_checks', []).append(dict(path=path, model_type=model_type))
        if 'broken' in path:
            return dict(ok=False, path=path, file_version='1', error='RKNN 模型无法加载')
        if model_type == 'yolov5':
            return dict(ok=False, path=path, file_version='1', error='yolov5 当前要求 3 个输出张量')
        return dict(ok=True, path=path, file_version='1', width=640, height=640, format='NHWC')

    @app.get(BASE + '/jobs/current')
    def current_job():
        state['current_requests'] = state.get('current_requests', 0) + 1
        return state.get('job')

    @app.delete(BASE + '/jobs/completed')
    def clear_completed():
        state['clear_requests'] = state.get('clear_requests', 0) + 1
        job = state.get('job')
        cleared = int(job is not None and job['status'] in ('completed', 'completed_with_errors', 'failed', 'cancelled'))
        if cleared:
            state.pop('job')
        return dict(cleared=cleared, current=state.get('job'))

    def batch(settings, names, source, job_id=None):
        state.update(settings=settings, names=names)
        state['job'] = dict(id=job_id or uuid.uuid4().hex, status=state.get('initial_status', 'completed'), created_at=time.time(), error='',
            total=len(names), completed=len(names), settings=settings, source=source,
            items=[dict(index=i, name=name, status='success', result=f'{i:06d}/result.json',
                        **({'source_path': source['path'] + '/' + name} if source['kind'] == 'device' else {}))
                   for i, name in enumerate(names)])
        # Persist simulated native output, then exercise the real label export route.
        manager = routes.image_jobs
        directory = manager.root / state['job']['id']
        directory.mkdir(parents=True)
        write_json(directory / 'status.json', state['job'])
        for index in range(len(names)):
            output = directory / 'output' / f'{index:06d}'
            output.mkdir(parents=True)
            write_json(output / 'result.json', result(state['job']['id'], index))
        manager._loaded = True
        manager._jobs[state['job']['id']] = Job(directory, state['job'])
        return state['job']

    @app.post(BASE + '/jobs/{job_id}/cancel')
    def cancel(job_id: str):
        assert state['job']['id'] == job_id
        state['job']['status'] = 'cancelled'
        return state['job']

    @app.post(BASE + '/jobs', status_code=202)
    async def create(request: Request):
        state['upload'] = await request.json()
        state['upload_id'] = uuid.uuid4().hex
        state['uploaded'] = {}
        return {'id': state['upload_id']}

    @app.put(BASE + '/jobs/{job_id}/inputs/{index}')
    async def upload_image(job_id: str, index: int, request: Request):
        assert job_id == state['upload_id']
        data = await request.body()
        assert len(data) == state['upload']['files'][index]['size']
        state['uploaded'][index] = len(data)
        return {'received': len(data)}

    @app.post(BASE + '/jobs/{job_id}/submit', status_code=202)
    def submit(job_id: str):
        upload = state['upload']
        assert len(state['uploaded']) == len(upload['files'])
        return batch(upload['settings'], [file['name'] for file in upload['files']], dict(kind='upload'), job_id)

    @app.delete(BASE + '/jobs/{job_id}/upload')
    def discard_upload(job_id: str):
        state['discarded_upload'] = job_id
        state['uploaded'] = {}
        return {'ok': True}

    @app.post(BASE + '/device-images')
    async def inspect(request: Request):
        data = await request.json()
        state['preview'] = data
        names = ['first.png', 'nested/second.png'] if data['recursive'] else ['first.png']
        return dict(**data, is_directory=True, total=len(names), total_bytes=100 * len(names),
                    images=[dict(name=name, path=data['path'] + '/' + name, size=100) for name in names])

    @app.post(BASE + '/jobs/from-device', status_code=202)
    async def create_device(request: Request):
        data = await request.json()
        state['device_request'] = data
        names = ['first.png', 'nested/second.png'] if data['recursive'] else ['first.png']
        return batch(data['settings'], names, dict(kind='device', path=data['path'], recursive=data['recursive'], is_directory=True))

    @app.get(BASE + '/jobs/{job}/items/{index}')
    def result(job: str, index: int):
        models = state['settings']['models']
        return dict(source_width=1280, source_height=720, coordinate_width=640, coordinate_height=640,
            elapsed_ms=25, original='original.jpg', annotated='annotated.jpg',
            models=[dict(id=m['id'], type=m['model_type'], succeeded=True, error='', input_width=640, input_height=640,
                         preprocess_ms=2, infer_ms=20, postprocess_ms=3, annotated=f'model_{i}.jpg') for i, m in enumerate(models)],
            results=[dict(model_id=m['id'], model_type=m['model_type'], label='person', class_id=0, score=.55,
                          track_id=-1, box=dict(x=100, y=200, width=80, height=140)) for m in models if m['obj_thresh'] <= .55])

    @app.get(BASE + '/jobs/{job}/files/{path:path}')
    def image(job: str, path: str): return Response(PNG, media_type='image/png')

    # Real FastAPI upload handling, size checks, atomic storage and duplicate naming.
    app.include_router(routes.router, prefix='/api')
    app.include_router(package_assets.router, prefix='/api')

    @app.api_route('/api/{path:path}', methods=['GET', 'POST'])
    def ancillary(path: str): return {}

    app.mount('/assets', StaticFiles(directory=FRONTEND / 'dist/assets'), name='assets')

    @app.get('/{path:path}')
    def page(path: str): return FileResponse(FRONTEND / 'dist/index.html')

    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    server = uvicorn.Server(uvicorn.Config(app, log_level='error', lifespan='off'))
    worker = threading.Thread(target=lambda: server.run(sockets=[listener]), daemon=True)
    worker.start()
    for _ in range(100):
        if server.started: break
        time.sleep(.02)
    assert server.started
    try:
        yield f'http://127.0.0.1:{listener.getsockname()[1]}', state
    finally:
        server.should_exit = True
        worker.join(timeout=5)
        listener.close()


def test_multi_model_upload_views_and_reload(console):
    url, state = console
    expect = playwright.expect
    with playwright.sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                                    headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
        context = browser.new_context(viewport={'width': 1500, 'height': 1000})
        context.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'test',username:'tester'},version:0}))")
        page = context.new_page()
        errors = []
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.goto(url + '/image-inference')
        page.get_by_label('模型 1 程序包', exact=True).select_option('demo')
        page.get_by_label('模型 1 文件', exact=True).select_option('/packages/demo/assets/first.rknn')
        page.get_by_label('模型 1 标签文件', exact=True).select_option('/packages/demo/assets/labels.txt')
        expect(page.get_by_label('模型 1 置信度阈值', exact=True)).not_to_be_visible()
        page.get_by_label('模型 1 参数', exact=True).click()
        page.get_by_label('模型 1 置信度阈值', exact=True).fill('0.2')
        page.get_by_role('button', name='＋ 添加模型', exact=False).click()
        page.get_by_label('模型 2 程序包', exact=True).select_option('demo')
        page.get_by_label('模型 2 文件', exact=True).select_option('/packages/demo/assets/second.rknn')
        page.get_by_label('模型 2 标签文件', exact=True).select_option('/packages/demo/assets/labels.txt')
        page.get_by_label('模型 2 参数', exact=True).click()
        page.get_by_label('模型 2 置信度阈值', exact=True).fill('0.9')
        # Each model folds independently and keeps its files and parameters.
        page.get_by_label('模型 1 设置', exact=True).click()
        expect(page.get_by_label('模型 1 文件', exact=True)).not_to_be_visible()
        expect(page.get_by_label('模型 2 文件', exact=True)).to_be_visible()
        page.get_by_label('模型 1 设置', exact=True).click()
        expect(page.get_by_label('模型 1 文件', exact=True)).to_have_value('/packages/demo/assets/first.rknn')
        expect(page.get_by_label('模型 1 置信度阈值', exact=True)).to_have_value('0.2')
        page.get_by_label('选择图片', exact=True).set_input_files([
            dict(name='first.png', mimeType='image/png', buffer=PNG), dict(name='second.png', mimeType='image/png', buffer=PNG)])
        # An invalid field in collapsed parameters must be revealed, not silently block submission.
        model_id = page.get_by_label('模型 2 ID', exact=True)
        model_id.fill('invalid id')
        page.get_by_label('模型 2 参数', exact=True).click()
        page.get_by_label('模型 2 设置', exact=True).click()
        page.get_by_role('button', name='开始推理 · 2 个模型', exact=True).click()
        expect(model_id).to_be_visible()
        assert 'settings' not in state
        model_id.fill('model_2')
        page.get_by_label('模型 2 参数', exact=True).click()
        page.get_by_label('模型 1 设置', exact=True).click()
        page.get_by_label('模型 2 设置', exact=True).click()
        page.get_by_role('button', name='开始推理 · 2 个模型', exact=True).click()
        expect(page.locator('.image-result-title')).to_contain_text('first.png')
        expect(page.get_by_role('region', name='详细结果', exact=True)).to_be_visible()
        expect(page.locator('.image-objects')).to_be_visible()
        expect(page.locator('.image-result details')).to_have_count(0)
        expect(page.locator('.image-objects')).to_contain_text('0.55')
        expect(page.get_by_role('link', name='下载原图', exact=True)).to_have_count(0)
        expect(page.get_by_role('link', name='下载推理图', exact=True)).to_have_count(0)
        expect(page.get_by_text('JSON', exact=False)).to_have_count(0)
        expect(page.locator('.image-result a[href*="result.json"], .image-result pre')).to_have_count(0)
        assert state['names'] == ['first.png', 'second.png']
        assert [m['obj_thresh'] for m in state['settings']['models']] == [.2, .9]
        assert [m['model_path'] for m in state['settings']['models']] == ['/packages/demo/assets/first.rknn', '/packages/demo/assets/second.rknn']
        assert_comparison(page)
        page.get_by_label('结果视图', exact=True).select_option('model:model_2')
        expect(page.locator('[data-view="inference"] img')).to_have_attribute('src', re.compile(r'model_1\.jpg'))
        expect(page.locator('.image-objects tbody tr')).to_have_count(0)
        expect(page.locator('[data-view="original"] img')).to_have_attribute('src', re.compile(r'original\.jpg'))
        page.get_by_label('结果视图', exact=True).select_option('all')
        expect(page.locator('[data-view="inference"] img')).to_have_attribute('src', re.compile(r'annotated\.jpg'))
        page.reload()
        page.get_by_label('模型 1 程序包', exact=True).select_option('demo')
        expect(page.locator('.image-result-title')).to_contain_text('first.png')
        expect(page.get_by_label('任务记录', exact=True)).to_have_count(0)
        expect(page.get_by_role('button', name='使用此任务参数', exact=True)).to_have_count(0)
        expect(page.get_by_role('link', name='下载整批结果', exact=True)).to_have_count(0)
        expect(page.get_by_role('button', name='下载标签 TXT', exact=False)).to_have_count(0)
        page.get_by_role('button', name='2. second.png', exact=False).click()
        expect(page.locator('.image-result-title')).to_contain_text('second.png')
        assert_comparison(page)
        expect(page.get_by_label('推理程序', exact=True)).to_have_count(0)
        assert state.get('app_requests', 0) >= 1
        assert errors == []
        browser.close()


def assert_comparison(page, models=2):
    expect = playwright.expect
    expect(page.locator('.image-stage')).to_have_count(2)
    expect(page.get_by_label('缩放', exact=False)).to_have_count(0)
    expect(page.locator('.image-result-tools select')).to_have_count(1 if models > 1 else 0)
    # Measure both in one browser frame; submission may still be scrolling smoothly.
    left, right = page.locator('.image-stage').evaluate_all('(elements) => elements.map(el => el.getBoundingClientRect().toJSON())')
    assert left['height'] == right['height'] == 360
    assert abs(left['width'] - right['width']) < 1
    assert abs(left['y'] - right['y']) < 1 and right['x'] > left['x'] + left['width']


def test_current_batch_cancel_reload_and_new_submission(console):
    url, state = console
    state['initial_status'] = 'running'
    expect = playwright.expect
    with playwright.sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                                    headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
        page = browser.new_page(viewport={'width': 1366, 'height': 900})
        page.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'test',username:'tester'},version:0}))")
        requests, errors = [], []
        page.on('request', lambda request: requests.append((request.method, request.url.split('?')[0])))
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.goto(url + '/image-inference')
        page.get_by_label('模型 1 程序包', exact=True).select_option('demo')
        expect(page.get_by_text('选择图片，开始推理', exact=True)).to_be_visible()
        page.get_by_label('模型 1 文件', exact=True).select_option('/packages/demo/assets/first.rknn')
        page.get_by_label('模型 1 标签文件', exact=True).select_option('/packages/demo/assets/labels.txt')
        page.get_by_label('选择图片', exact=True).set_input_files([
            dict(name=name, mimeType='image/png', buffer=PNG) for name in ['first.png', 'second.png']])
        page.get_by_role('button', name='开始推理 · 1 个模型', exact=True).click()
        expect(page.get_by_role('button', name='取消', exact=True)).to_be_visible()
        expect(page.get_by_role('button', name='清除记录', exact=True)).to_have_count(0)
        expect(page.get_by_role('button', name='开始推理 · 1 个模型', exact=True)).to_be_disabled()
        first_id = state['job']['id']
        page.get_by_role('button', name='取消', exact=True).click()
        expect(page.get_by_role('button', name='取消', exact=True)).to_have_count(0)
        expect(page.get_by_role('button', name='清除记录', exact=True)).to_be_visible()
        expect(page.get_by_role('link', name='下载整批结果', exact=True)).to_have_count(0)
        expect(page.get_by_role('button', name='下载标签 TXT', exact=False)).to_be_disabled()
        page.reload()
        page.get_by_label('模型 1 程序包', exact=True).select_option('demo')
        expect(page.locator('.image-result-title')).to_contain_text('first.png')
        page.get_by_role('button', name='2. second.png', exact=False).click()
        expect(page.locator('.image-result-title')).to_contain_text('second.png')
        state['initial_status'] = 'completed'
        page.get_by_label('模型 1 文件', exact=True).select_option('/packages/demo/assets/first.rknn')
        page.get_by_label('模型 1 标签文件', exact=True).select_option('/packages/demo/assets/labels.txt')
        page.get_by_label('选择图片', exact=True).set_input_files(dict(name='next.png', mimeType='image/png', buffer=PNG))
        page.get_by_role('button', name='开始推理 · 1 个模型', exact=True).click()
        expect(page.locator('.image-result-title')).to_contain_text('next.png')
        expect(page.get_by_label('选择标签 1. next.png', exact=True)).not_to_be_checked()
        assert state['job']['id'] != first_id
        page.reload()
        page.get_by_label('模型 1 程序包', exact=True).select_option('demo')
        expect(page.locator('.image-result-title')).to_contain_text('next.png')
        assert state['current_requests'] >= 3
        assert ('GET', url + BASE + '/jobs') not in requests
        assert not errors
        browser.close()


def test_clear_records_retry_stale_poll_reload_and_new_submission(console):
    url, state = console
    expect = playwright.expect
    with playwright.sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                                    headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
        page = browser.new_page(viewport={'width': 1366, 'height': 900})
        page.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'test',username:'tester'},version:0}))")
        errors, delete_requests = [], []
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.on('request', lambda request: delete_requests.append(request.url) if request.method == 'DELETE' else None)
        page.goto(url + '/image-inference')
        page.get_by_label('模型 1 程序包', exact=True).select_option('demo')
        clear = page.get_by_role('button', name='清除记录', exact=True)
        expect(clear).to_have_count(0)
        page.get_by_label('模型 1 文件', exact=True).select_option('/packages/demo/assets/first.rknn')
        page.get_by_label('模型 1 标签文件', exact=True).select_option('/packages/demo/assets/labels.txt')
        page.get_by_label('模型 1 参数', exact=True).click()
        page.get_by_label('模型 1 置信度阈值', exact=True).fill('0.2')
        page.get_by_label('选择图片', exact=True).set_input_files([
            dict(name=name, mimeType='image/png', buffer=PNG) for name in ['first.png', 'second.png']])
        page.get_by_role('button', name='开始推理 · 1 个模型', exact=True).click()
        expect(clear).to_be_visible()
        expect(clear).to_have_text('清除记录')
        page.get_by_role('button', name='2. second.png', exact=False).click()
        expect(page.locator('.image-result-title')).to_contain_text('second.png')
        old_job = json.dumps(state['job'])
        dialogs = []
        def dismiss_clear(dialog):
            dialogs.append((dialog.type, dialog.message))
            assert delete_requests == []
            dialog.dismiss()
        page.once('dialog', dismiss_clear)
        clear.click()
        assert len(dialogs) == 1 and dialogs[0][0] == 'confirm'
        assert all(text in dialogs[0][1] for text in ['全部已结束', '无法恢复', '板端原图'])
        expect(page.locator('.image-result-title')).to_contain_text('second.png')
        expect(clear).to_be_enabled()
        assert json.dumps(state['job']) == old_job
        assert delete_requests == [] and state.get('clear_requests', 0) == 0
        page.route('**/jobs/completed', lambda route: route.fulfill(status=503, content_type='application/json',
                   body=json.dumps({'detail': '图片任务存储不可用：Permission denied'})), times=1)
        page.once('dialog', lambda dialog: dialog.accept())
        clear.click()
        expect(page.get_by_role('alert')).to_contain_text('存储不可用')
        expect(page.locator('.image-result-title')).to_contain_text('second.png')
        expect(clear).to_be_enabled()
        assert state.get('clear_requests', 0) == 0

        # Keep one old current-job response in flight across successful cleanup.
        pending = []
        page.route('**/jobs/current', lambda route: pending.append(route), times=1)
        deadline = time.monotonic() + 3
        while not pending and time.monotonic() < deadline:
            page.wait_for_timeout(20)
        assert pending
        page.once('dialog', lambda dialog: dialog.accept())
        clear.click()
        expect(page.get_by_text('选择图片，开始推理', exact=True)).to_be_visible()
        assert state['clear_requests'] == 1 and 'job' not in state
        assert len(delete_requests) == 2
        pending.pop().fulfill(status=200, content_type='application/json', body=old_job)
        page.evaluate('() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)))')
        expect(page.locator('.image-result-title')).to_have_count(0)
        expect(page.get_by_role('link', name='下载整批结果', exact=True)).to_have_count(0)
        expect(clear).to_have_count(0)
        expect(page.get_by_role('alert')).to_have_count(0)
        expect(page.get_by_label('模型 1 文件', exact=True)).to_have_value('/packages/demo/assets/first.rknn')
        expect(page.get_by_label('模型 1 标签文件', exact=True)).to_have_value('/packages/demo/assets/labels.txt')
        expect(page.get_by_label('模型 1 置信度阈值', exact=True)).to_have_value('0.2')
        assert page.get_by_label('选择图片', exact=True).evaluate('el => el.files.length') == 2
        page.reload()
        page.get_by_label('模型 1 程序包', exact=True).select_option('demo')
        expect(page.get_by_label('模型 1 文件', exact=True)).to_be_visible()
        expect(page.get_by_text('选择图片，开始推理', exact=True)).to_be_visible()
        expect(page.locator('.image-result-title')).to_have_count(0)
        page.get_by_label('模型 1 文件', exact=True).select_option('/packages/demo/assets/first.rknn')
        page.get_by_label('模型 1 标签文件', exact=True).select_option('/packages/demo/assets/labels.txt')
        page.get_by_label('选择图片', exact=True).set_input_files(dict(name='next.png', mimeType='image/png', buffer=PNG))
        page.get_by_role('button', name='开始推理 · 1 个模型', exact=True).click()
        expect(page.locator('.image-result-title')).to_contain_text('next.png')
        expect(clear).to_be_visible()
        assert not errors
        browser.close()


def test_device_folder_preview_submission_and_reload(console):
    url, state = console
    expect = playwright.expect
    with playwright.sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                                    headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
        context = browser.new_context(viewport={'width': 1500, 'height': 1000})
        context.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'test',username:'tester'},version:0}))")
        page = context.new_page()
        errors = []
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.goto(url + '/image-inference')
        page.get_by_label('模型 1 程序包', exact=True).select_option('demo')
        page.get_by_label('模型 1 文件', exact=True).select_option('/packages/demo/assets/first.rknn')
        page.get_by_label('模型 1 标签文件', exact=True).select_option('/packages/demo/assets/labels.txt')
        page.get_by_label('模型 1 参数', exact=True).click()
        page.get_by_label('模型 1 置信度阈值', exact=True).fill('0.2')
        page.get_by_label('选择图片来源', exact=True).select_option(label='板端文件')
        page.get_by_label('板端图片或文件夹路径', exact=True).fill('/userdata/测试图片')
        page.get_by_role('button', name='读取图片清单', exact=True).click()
        expect(page.locator('.image-device-preview')).to_contain_text('找到 1 张图片')
        page.get_by_label('包括子文件夹', exact=True).check()
        expect(page.locator('.image-device-preview')).to_have_count(0)
        page.get_by_role('button', name='读取图片清单', exact=True).click()
        expect(page.locator('.image-device-preview')).to_contain_text('找到 2 张图片')
        assert state['preview'] == dict(path='/userdata/测试图片', recursive=True)
        page.get_by_role('button', name='开始推理 · 1 个模型', exact=True).click()
        expect(page.locator('.image-result-title')).to_contain_text('first.png')
        expect(page.locator('.image-result .image-source-origin')).to_be_visible()
        expect(page.locator('.image-result .image-source-origin')).to_contain_text('/userdata/测试图片/first.png')
        assert state['device_request']['path'] == '/userdata/测试图片'
        assert state['device_request']['recursive'] is True
        assert state['settings']['models'][0]['obj_thresh'] == .2
        assert_comparison(page, models=1)
        page.reload()
        page.get_by_label('模型 1 程序包', exact=True).select_option('demo')
        expect(page.locator('.image-result-title')).to_contain_text('first.png')
        page.get_by_role('button', name='2. nested/second.png', exact=False).click()
        expect(page.locator('.image-result-title')).to_contain_text('nested/second.png')
        assert_comparison(page, models=1)
        page.get_by_label('选择图片来源', exact=True).select_option('upload')
        expect(page.get_by_label('选择图片', exact=True)).to_be_visible()
        expect(page.get_by_label('板端图片或文件夹路径', exact=True)).to_have_count(0)
        expect(page.get_by_label('推理程序', exact=True)).to_have_count(0)
        assert state.get('app_requests', 0) >= 1
        assert errors == []
        browser.close()


def test_canvas_preset_includes_all_enabled_channel_models():
    script = r"""
const assert = require('node:assert/strict'), fs = require('node:fs'), ts = require('typescript');
require.extensions['.ts'] = (m, name) => m._compile(ts.transpileModule(fs.readFileSync(name, 'utf8'), {
 compilerOptions: {module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020}}).outputText, name);
const {channelImageSettings} = require('./src/features/image-inference/preset.ts');
const roi = {mode:'roi_only', resize_mode:'letterbox', polygon:[[0,0],[1,0],[1,1]]};
const nodes = [{id:'s1',type:'stream',data:{inference_roi:roi}}, {id:'s2',type:'stream',data:{}},
 {id:'m1',type:'model',data:{id:'person',obj_thresh:.9}},
 {id:'m2',type:'model',data:{id:'pose',model_type:'yolov8_pose',obj_thresh:.6}},
 {id:'m3',type:'model',data:{id:'disabled',enable:false}},
 {id:'m4',type:'model',data:{id:'other'}}];
const edges = ['m1','m2','m3','m4'].map((target,i) => ({source:i===3?'s2':'s1', target, sourceHandle:'stream-out',targetHandle:'stream-in'}));
const settings = channelImageSettings(nodes[2], nodes, edges);
assert.deepEqual(settings.models.map(m=>m.id), ['person','pose']);
assert.deepEqual(settings.models.map(m=>m.obj_thresh), [.9,.6]);
assert.deepEqual(settings.inference_roi, roi);
assert.equal(nodes[2].data.obj_thresh, .9);
global.localStorage = {getItem(key) {return this[key] || null}, setItem(key,value) {this[key]=value}, removeItem(key) {delete this[key]}};
let opened;
global.window = {open(url) {opened=url}};
const {openImageInference,readImagePreset} = require('./src/features/image-inference/preset.ts');
openImageInference('canvas-demo', settings);
const query = new URL(opened, 'http://console').searchParams;
assert.equal(query.has('app'), false);
assert.equal(readImagePreset(query.get('preset')).app, 'canvas-demo');
assert.deepEqual(readImagePreset(query.get('preset')).settings.models.map(m=>m.id), ['person','pose']);
"""
    subprocess.run(['node', '-e', script], cwd=FRONTEND, check=True, timeout=30)


def test_model_selection_validation_retry_and_late_response(console):
    url, state = console
    expect = playwright.expect
    with playwright.sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                                    headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
        page = browser.new_page(viewport={'width': 1366, 'height': 900})
        page.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'test',username:'tester'},version:0}))")
        errors = []
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.goto(url + '/image-inference')
        page.get_by_label('模型 1 程序包', exact=True).select_option('demo')
        model = page.get_by_label('模型 1 文件', exact=True)
        kind = page.get_by_label('模型 1 类型', exact=True)
        check = page.get_by_role('status', name='模型 1 检查', exact=True)
        start = page.get_by_role('button', name='开始推理 · 1 个模型', exact=True)
        model.select_option('/packages/demo/assets/first.rknn')
        page.get_by_label('模型 1 标签文件', exact=True).select_option('/packages/demo/assets/labels.txt')
        page.get_by_label('选择图片', exact=True).set_input_files(dict(name='image.png', mimeType='image/png', buffer=PNG))
        expect(check).to_contain_text('640 × 640 · NHWC')
        expect(start).to_be_enabled()
        kind.select_option('yolov5')
        expect(check).to_contain_text('3 个输出张量')
        expect(start).to_be_disabled()
        assert 'job' not in state

        # An older successful check cannot authorize a newly incompatible selection.
        pending = []
        page.route('**/models/info?*', lambda route: pending.append(route), times=1)
        kind.select_option('yolov8_det')
        deadline = time.monotonic() + 3
        while not pending and time.monotonic() < deadline:
            page.wait_for_timeout(20)
        assert pending
        expect(check).to_contain_text('检查中')
        expect(start).to_be_disabled()
        kind.select_option('yolov5')
        expect(check).to_contain_text('3 个输出张量')
        pending.pop().fulfill(json=dict(ok=True, path='/packages/demo/assets/first.rknn', file_version='old', width=960, height=960, format='NHWC'))
        page.evaluate('() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)))')
        expect(check).to_contain_text('3 个输出张量')
        expect(start).to_be_disabled()
        kind.select_option('yolov8_det')
        expect(check).to_contain_text('640 × 640')

        model.select_option('/packages/demo/assets/broken.rknn')
        expect(check).to_contain_text('无法加载')
        expect(start).to_be_disabled()
        model.select_option('/packages/demo/assets/valid.rknn')
        expect(check).to_contain_text('640 × 640')
        expect(start).to_be_enabled()
        assert any(item['path'].endswith('/valid.rknn') for item in state['model_checks'])

        page.route('**/models/info?*', lambda route: route.fulfill(status=503, json={'detail': '模型检查暂不可用'}), times=1)
        model.select_option('/packages/demo/assets/second.rknn')
        expect(check).to_contain_text('模型检查暂不可用')
        expect(start).to_be_disabled()
        page.get_by_role('button', name='重新检查模型 1', exact=True).click()
        expect(check).to_contain_text('640 × 640')
        expect(start).to_be_enabled()
        page.get_by_role('button', name='添加模型', exact=False).click()
        page.get_by_label('模型 2 程序包', exact=True).select_option('demo')
        page.get_by_label('模型 2 文件', exact=True).select_option('/packages/demo/assets/first.rknn')
        page.get_by_label('模型 2 类型', exact=True).select_option('yolov5')
        expect(page.get_by_role('status', name='模型 2 检查', exact=True)).to_contain_text('3 个输出张量')
        expect(page.get_by_role('button', name='开始推理 · 2 个模型', exact=True)).to_be_disabled()
        page.get_by_role('button', name='移除模型 2', exact=True).click()
        expect(start).to_be_enabled()
        start.click()
        expect(page.locator('.image-result-title')).to_contain_text('image.png')
        assert not errors
        browser.close()


def test_package_selection_and_optional_canvas_adapter(console):
    url, state = console
    expect = playwright.expect
    with playwright.sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                                    headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
        context = browser.new_context(viewport={'width': 1500, 'height': 1000})
        context.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'test',username:'tester'},version:0}))")
        page = context.new_page(); errors = []
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.goto(url + '/image-inference')
        page.get_by_label('模型 1 程序包', exact=True).select_option('demo')
        page.get_by_label('模型 1 文件', exact=True).select_option('/packages/demo/assets/first.rknn')
        page.get_by_label('模型 1 标签文件', exact=True).select_option('/packages/demo/assets/labels.txt')
        page.get_by_label('选择图片', exact=True).set_input_files(dict(name='image.png', mimeType='image/png', buffer=PNG))
        page.get_by_role('button', name='开始推理 · 1 个模型', exact=True).click()
        expect(page.locator('.image-objects')).to_contain_text('0.55')
        assert state['settings']['models'][0]['model_path'] == '/packages/demo/assets/first.rknn'
        preset = dict(app='canvas-demo', settings=dict(models=[dict(state['settings']['models'][0],
            model_path='assets/canvas.rknn', label_path='assets/canvas.txt', obj_thresh=.9)]), created=time.time()*1000)
        page.evaluate('(value) => localStorage.setItem("rk3588.image-preset.example", JSON.stringify(value))', preset)
        page.goto(url + '/image-inference?preset=example')
        expect(page.get_by_label('模型 1 程序包', exact=True)).to_have_value('canvas-demo')
        expect(page.get_by_label('模型 1 文件', exact=True)).to_have_value('/canvas/assets/canvas.rknn')
        expect(page.get_by_label('模型 1 置信度阈值', exact=True)).to_have_value('0.9')
        assert state['preset']['app'] == 'canvas-demo'
        expect(page.get_by_label('推理程序', exact=True)).to_have_count(0)
        assert state.get('app_requests', 0) >= 1
        assert errors == []
        browser.close()


@pytest.mark.parametrize('viewport', [{'width': 1366, 'height': 768}, {'width': 768, 'height': 740}])
def test_page_scroll_reaches_model_controls_and_results(console, viewport):
    url, _ = console
    expect = playwright.expect
    with playwright.sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                                    headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
        page = browser.new_page(viewport=viewport)
        page.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'test',username:'tester'},version:0}))")
        page.goto(url + '/image-inference')
        page.get_by_label('模型 1 程序包', exact=True).select_option('demo')
        expect(page.get_by_label('模型 1 文件', exact=True)).to_be_visible()
        page.get_by_label('模型 1 设置', exact=True).click()
        expect(page.get_by_label('模型 1 文件', exact=True)).not_to_be_visible()
        page.get_by_label('模型 1 设置', exact=True).click()
        if viewport['width'] <= 1000:
            expect(page.get_by_role('separator', name='调整模型配置宽度')).not_to_be_visible()
        panel = page.locator('.image-inference-page')
        expect(page.get_by_label('模型 1 置信度阈值', exact=True)).not_to_be_visible()
        expect(page.get_by_label('任务记录', exact=True)).to_have_count(0)
        expect(page.get_by_role('button', name='删除任务', exact=True)).to_have_count(0)
        if viewport['width'] > 1000:
            expect(page.get_by_role('button', name='开始推理 · 1 个模型', exact=True)).to_be_in_viewport()
        add = page.get_by_role('button', name='＋ 添加模型', exact=False)
        add.click()
        expect(page.get_by_label('模型 2 文件', exact=True)).to_have_count(0)
        page.get_by_label('模型 2 程序包', exact=True).select_option('demo')
        expect(page.get_by_label('模型 2 文件', exact=True)).to_be_visible()
        # Longer multi-model content still scrolls after compacting the default view.
        metrics = panel.evaluate('el => ({overflow:getComputedStyle(el).overflowY, height:el.clientHeight, total:el.scrollHeight})')
        assert metrics['overflow'] in ('auto', 'scroll') and metrics['total'] > metrics['height']
        page.mouse.move(viewport['width'] - 60, 200)
        page.mouse.wheel(0, 650)
        page.wait_for_function('document.querySelector(".image-inference-page").scrollTop > 0')
        start = page.get_by_role('button', name='开始推理 · 2 个模型', exact=True)
        start.scroll_into_view_if_needed()
        expect(start).to_be_in_viewport()
        page.get_by_label('选择图片', exact=True).scroll_into_view_if_needed()
        expect(page.get_by_label('选择图片', exact=True)).to_be_in_viewport()
        results = page.get_by_role('heading', name='结果', exact=True)
        results.scroll_into_view_if_needed()
        expect(results).to_be_in_viewport()
        assert panel.evaluate('el => el.scrollWidth <= el.clientWidth')
        assert page.evaluate('document.documentElement.scrollWidth <= window.innerWidth')
        browser.close()


@pytest.mark.parametrize('viewport', [{'width': 1366, 'height': 768}, {'width': 768, 'height': 740}])
def test_collapsible_config_and_vertical_result_list(console, viewport):
    url, state = console
    expect = playwright.expect
    with playwright.sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                                    headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'],
                                    ignore_default_args=['--hide-scrollbars'])
        page = browser.new_page(viewport=viewport)
        page.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'test',username:'tester'},version:0}))")
        errors = []
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.goto(url + '/image-inference')
        page.get_by_label('模型 1 程序包', exact=True).select_option('demo')
        model = page.get_by_label('模型 1 文件', exact=True)
        model.select_option('/packages/demo/assets/first.rknn')
        page.get_by_label('模型 1 标签文件', exact=True).select_option('/packages/demo/assets/labels.txt')
        page.get_by_label('模型 1 参数', exact=True).click()
        threshold = page.get_by_label('模型 1 置信度阈值', exact=True)
        threshold.fill('0.37')
        page.get_by_label('模型 1 参数', exact=True).click()
        page.get_by_label('选择图片', exact=True).set_input_files([
            dict(name=f'batch-{i:02d}.png', mimeType='image/png', buffer=PNG) for i in range(1, 41)])
        page.get_by_role('button', name='收起模型配置', exact=True).click()
        expect(page.locator('.image-config')).to_be_hidden()
        expand = page.get_by_role('button', name='展开模型配置', exact=True)
        expect(expand).to_have_attribute('aria-expanded', 'false')
        expand.click()
        expect(model).to_have_value('/packages/demo/assets/first.rknn')
        expect(threshold).to_have_value('0.37')
        assert page.get_by_label('选择图片', exact=True).evaluate('el => el.files.length') == 40
        page.get_by_role('button', name='开始推理 · 1 个模型', exact=True).click()
        expect(page.locator('.image-result-title')).to_contain_text('batch-01.png')
        assert state['settings']['models'][0]['obj_thresh'] == .37
        preview = page.locator('.image-result-preview')
        original_width = preview.bounding_box()['width']
        page.get_by_role('button', name='收起模型配置', exact=True).click()
        if viewport['width'] > 1000:
            assert preview.bounding_box()['width'] > original_width + 200
        panel = page.locator('.image-inference-page')
        panel.evaluate('el => {el.scrollTop = 0}')
        listing = page.get_by_role('group', name='批量图片列表', exact=True)
        expect(listing.get_by_role('button')).to_have_count(40)
        preview_box, list_box = page.locator('.image-result-preview, .image-item-list').evaluate_all(
            'els => els.map(el => el.getBoundingClientRect().toJSON())')
        assert list_box['x'] > preview_box['right']
        rows = listing.get_by_role('button').evaluate_all('els => els.slice(0, 2).map(el => el.getBoundingClientRect().toJSON())')
        assert rows[0]['x'] == rows[1]['x'] and rows[1]['y'] >= rows[0]['bottom']
        assert rows[0]['height'] <= 40 and rows[0]['width'] > rows[0]['height'] * 3
        metrics = listing.evaluate('el => ({height:el.clientHeight, total:el.scrollHeight, overflow:getComputedStyle(el).overflowY})')
        assert metrics['overflow'] in ('auto', 'scroll') and metrics['total'] > metrics['height']
        listing.hover()
        outer_scroll = panel.evaluate('el => el.scrollTop')
        page.mouse.wheel(0, 700)
        page.wait_for_function('document.querySelector(".image-item-list").scrollTop > 0')
        assert panel.evaluate('el => el.scrollTop') == outer_scroll
        last = listing.get_by_role('button', name='40. batch-40.png', exact=False)
        last.click()
        expect(last).to_have_attribute('aria-pressed', 'true')
        expect(listing.locator('[aria-pressed="true"]')).to_have_count(1)
        expect(page.locator('.image-result-title')).to_contain_text('batch-40.png')
        expect(page.locator('[data-view="original"] img')).to_have_attribute('src', re.compile(r'/000039/original\.jpg'))
        expect(page.locator('[data-view="inference"] img')).to_have_attribute('src', re.compile(r'/000039/annotated\.jpg'))
        page.get_by_role('button', name='展开模型配置', exact=True).click()
        expect(model).to_have_value('/packages/demo/assets/first.rknn')
        expect(page.locator('.image-result-title')).to_contain_text('batch-40.png')
        assert panel.evaluate('el => el.scrollWidth <= el.clientWidth')
        assert errors == []
        browser.close()


@pytest.mark.parametrize('width', [1366, 1100])
def test_config_edge_drag_resize_collapse_and_restore(console, width):
    url, state = console
    expect = playwright.expect
    with playwright.sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                                    headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
        page = browser.new_page(viewport={'width': width, 'height': 900})
        page.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'test',username:'tester'},version:0}))")
        errors = []
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.goto(url + '/image-inference')
        page.get_by_label('模型 1 程序包', exact=True).select_option('demo')
        model = page.get_by_label('模型 1 文件', exact=True)
        model.select_option('/packages/demo/assets/first.rknn')
        page.get_by_label('模型 1 标签文件', exact=True).select_option('/packages/demo/assets/labels.txt')
        page.get_by_label('模型 1 参数', exact=True).click()
        threshold = page.get_by_label('模型 1 置信度阈值', exact=True)
        threshold.fill('0.37')
        page.get_by_label('模型 1 参数', exact=True).click()
        page.get_by_label('选择图片', exact=True).set_input_files([
            dict(name=name, mimeType='image/png', buffer=PNG) for name in ['first.png', 'second.png']])
        page.get_by_role('button', name='开始推理 · 1 个模型', exact=True).click()
        page.get_by_role('button', name='2. second.png', exact=False).click()
        expect(page.locator('.image-result-title')).to_contain_text('second.png')
        page.locator('.image-inference-page').evaluate('el => {el.scrollTop=0}')
        separator = page.get_by_role('separator', name='调整模型配置宽度', exact=True)
        config = page.locator('.image-config')
        original_style = page.evaluate('({cursor:document.body.style.cursor,select:document.body.style.userSelect})')

        def drag(delta, release=True):
            box = separator.bounding_box()
            x, y = box['x'] + box['width'] / 2, box['y'] + 60
            page.mouse.move(x, y)
            page.mouse.down()
            # Leave the handle and its vertical bounds to exercise pointer capture.
            page.mouse.move(x + delta, 30, steps=8)
            if release:
                page.mouse.up()
                assert page.evaluate('({cursor:document.body.style.cursor,select:document.body.style.userSelect})') == original_style

        initial = config.bounding_box()['width']
        drag(80)
        assert abs(config.bounding_box()['width'] - initial - 80) <= 1
        drag(-140)
        expect(config).to_be_visible()
        assert config.bounding_box()['width'] >= 240
        drag(-140)
        expect(config).to_be_hidden()
        expect(separator).to_be_visible()
        expect(separator).to_have_attribute('aria-valuenow', '0')
        expect(page.get_by_role('button', name='展开模型配置', exact=True)).to_be_visible()
        drag(320)
        expect(config).to_be_visible()
        assert abs(config.bounding_box()['width'] - 320) <= 1
        expect(model).to_have_value('/packages/demo/assets/first.rknn')
        expect(threshold).to_have_value('0.37')
        assert page.get_by_label('选择图片', exact=True).evaluate('el => el.files.length') == 2
        expect(page.locator('.image-result-title')).to_contain_text('second.png')
        expect(page.locator('[data-view="inference"] img')).to_have_attribute('src', re.compile('/000001/annotated.jpg'))
        page.get_by_role('button', name='收起模型配置', exact=True).click()
        page.get_by_role('button', name='展开模型配置', exact=True).click()
        assert abs(config.bounding_box()['width'] - 320) <= 1
        drag(1600)
        assert abs(config.bounding_box()['width'] - float(separator.get_attribute('aria-valuemax'))) <= 1
        assert page.locator('.image-results-area').bounding_box()['width'] >= 419
        separator.press('Home')
        expect(config).to_be_hidden()
        separator.press('ArrowRight')
        expect(config).to_be_visible()
        separator.press('ArrowLeft')
        assert float(separator.get_attribute('aria-valuenow')) < float(separator.get_attribute('aria-valuemax'))
        drag(-20, release=False)
        page.evaluate("window.dispatchEvent(new Event('blur'))")
        assert page.evaluate('({cursor:document.body.style.cursor,select:document.body.style.userSelect})') == original_style
        page.mouse.up()
        drag(20, release=False)
        page.set_viewport_size({'width': 768, 'height': 740})
        expect(separator).not_to_be_visible()
        assert page.evaluate('({cursor:document.body.style.cursor,select:document.body.style.userSelect})') == original_style
        page.mouse.up()
        expect(model).to_have_value('/packages/demo/assets/first.rknn')
        assert page.locator('.image-inference-page').evaluate('el => el.scrollWidth <= el.clientWidth')
        assert not errors
        browser.close()


def test_select_and_download_yolo_labels_through_real_backend(console):
    url, state = console
    expect = playwright.expect
    with playwright.sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                                    headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
        page = browser.new_page(viewport={'width': 1500, 'height': 1000})
        page.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'test',username:'tester'},version:0}))")
        errors = []
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.goto(url + '/image-inference')
        page.get_by_label('模型 1 程序包', exact=True).select_option('demo')
        page.get_by_label('模型 1 文件', exact=True).select_option('/packages/demo/assets/first.rknn')
        page.get_by_label('模型 1 标签文件', exact=True).select_option('/packages/demo/assets/labels.txt')
        page.get_by_label('选择图片', exact=True).set_input_files([
            dict(name=name, mimeType='image/png', buffer=PNG) for name in ['first.png', 'second.png', 'failed.png']])
        page.get_by_role('button', name='开始推理 · 1 个模型', exact=True).click()
        export = page.get_by_role('button', name='下载标签 TXT', exact=False)
        expect(export).to_be_disabled()
        # Failed images remain viewable but cannot be selected as annotations.
        state['job']['items'][2]['status'] = 'failed'
        expect(page.get_by_label('选择标签 3. failed.png', exact=True)).to_be_disabled()
        page.get_by_label('选择标签 2. second.png', exact=True).check()
        expect(export).to_have_text('下载标签 TXT (1)')
        expect(page.locator('.image-result-title')).to_contain_text('first.png')
        select_all = page.get_by_label('全选检测结果', exact=True)
        assert select_all.evaluate('el => el.indeterminate')
        with page.expect_download() as download_info:
            export.click()
        download = download_info.value
        assert download.suggested_filename.startswith('yolo-labels-')
        with zipfile.ZipFile(download.path()) as archive:
            assert archive.namelist() == ['second.txt']
            fields = archive.read('second.txt').decode().split()
            assert len(fields) == 5 and fields[0] == '0'
            assert [float(value) for value in fields[1:]] == pytest.approx([140/640, 270/640, 80/640, 140/640])
        select_all.check()
        expect(export).to_have_text('下载标签 TXT (2)')
        # Editing the next run's models must not change the recorded job's eligibility.
        page.get_by_role('button', name='＋ 添加模型', exact=False).click()
        with page.expect_download() as download_info:
            export.click()
        with zipfile.ZipFile(download_info.value.path()) as archive:
            assert archive.namelist() == ['first.txt', 'second.txt']
        select_all.uncheck()
        expect(export).to_be_disabled()
        page.get_by_label('选择标签 1. first.png', exact=True).check()
        page.route('**/jobs/*/labels', lambda route: route.fulfill(status=422, content_type='application/json',
                   body=json.dumps({'detail': '结果数据不完整'})), times=1)
        export.click()
        expect(page.get_by_role('alert')).to_contain_text('结果数据不完整')
        expect(export).to_be_enabled()
        # Both previews fill their stages, including after resizing the window.
        for width in (1500, 768):
            page.set_viewport_size({'width': width, 'height': 1000})
            for stage in page.locator('.image-stage').all():
                geometry = stage.evaluate('''el => {
                    const image = el.querySelector('img');
                    return {fit:getComputedStyle(image).objectFit, width:image.clientWidth,
                            height:image.clientHeight, stageWidth:el.clientWidth, stageHeight:el.clientHeight};
                }''')
                assert geometry['fit'] == 'cover'
                assert geometry['width'] == geometry['stageWidth'] and geometry['height'] == geometry['stageHeight']
        expect(page.get_by_role('link', name='下载原图')).to_have_count(0)
        expect(page.get_by_role('link', name='下载推理图')).to_have_count(0)
        expect(page.get_by_role('button', name='清除记录', exact=True)).to_have_text('清除记录')
        page.reload()
        page.get_by_label('模型 1 程序包', exact=True).select_option('demo')
        expect(page.get_by_label('选择标签 1. first.png', exact=True)).not_to_be_checked()
        expect(export).to_be_disabled()
        assert not errors
        browser.close()


def test_package_cascade_keeps_models_and_labels_together(console):
    url, state = console
    expect = playwright.expect
    with playwright.sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                                    headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
        page = browser.new_page(viewport={'width': 1366, 'height': 900})
        page.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'test',username:'tester'},version:0}))")
        requests = []
        page.on('request', lambda request: requests.append(request.url))
        page.goto(url + '/image-inference')
        package = page.get_by_label('模型 1 程序包', exact=True)
        model = page.get_by_label('模型 1 文件', exact=True)
        label = page.get_by_label('模型 1 标签文件', exact=True)
        expect(package).to_be_visible()
        expect(model).to_have_count(0)
        expect(page.get_by_label('上传模型 1 文件', exact=True)).to_have_count(0)
        package.select_option('demo')
        expect(model.locator('option')).to_have_text(['选择模型', 'first.rknn', 'second.rknn', 'broken.rknn', 'valid.rknn', '导入模型…'])
        model.select_option('/packages/demo/assets/first.rknn')
        label.select_option('/packages/demo/assets/labels.txt')
        page.get_by_label('选择图片', exact=True).set_input_files(dict(name='image.png', mimeType='image/png', buffer=PNG))
        expect(page.get_by_role('button', name='开始推理 · 1 个模型', exact=True)).to_be_enabled()
        package.select_option('other')
        expect(model).to_have_value('')
        expect(label).to_have_value('')
        expect(page.get_by_role('button', name='开始推理 · 1 个模型', exact=True)).to_be_disabled()
        expect(model.locator('option')).to_have_text(['选择模型', 'first.rknn', '导入模型…'])
        expect(label.locator('option')).to_have_text(['选择标签', 'other.txt'])
        model.select_option('/packages/other/assets/first.rknn')
        label.select_option('/packages/other/assets/other.txt')
        page.get_by_role('button', name='添加模型', exact=False).click()
        page.get_by_label('模型 2 程序包', exact=True).select_option('demo')
        page.get_by_label('模型 2 文件', exact=True).select_option('/packages/demo/assets/second.rknn')
        page.get_by_label('模型 2 标签文件', exact=True).select_option('/packages/demo/assets/labels.txt')
        expect(package).to_have_value('other')
        page.get_by_role('button', name='开始推理 · 2 个模型', exact=True).click()
        expect(page.locator('.image-result-title')).to_contain_text('image.png')
        assert [m['model_path'] for m in state['settings']['models']] == [
            '/packages/other/assets/first.rknn', '/packages/demo/assets/second.rknn']
        package.select_option('empty')
        expect(model.locator('option')).to_have_text(['该程序包暂无 .rknn 文件', '导入模型…'])
        expect(page.get_by_role('button', name='开始推理 · 2 个模型', exact=True)).to_be_disabled()
        state['packages'] = []
        page.reload()
        expect(package.locator('option')).to_have_text(['暂无程序包，请先安装'])
        expect(model).to_have_count(0)
        assert not any('/image-inference/assets' in url for url in requests)
        # Recover from a package-list failure without silently reusing stale choices.
        page.route('**/api/apps?*', lambda route: route.fulfill(status=503, json={'detail': '程序包列表读取失败'}), times=1)
        page.reload()
        expect(page.get_by_role('alert')).to_contain_text('程序包列表读取失败')
        state.pop('packages')
        page.get_by_role('button', name='重试', exact=True).click()
        expect(package.locator('option')).to_have_count(5)
        browser.close()


def test_import_model_from_dropdown_into_selected_package(console):
    url, state = console
    expect = playwright.expect
    with playwright.sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                                    headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
        page = browser.new_page(viewport={'width': 1366, 'height': 900})
        page.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'test',username:'tester'},version:0}))")
        errors = []
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.goto(url + '/image-inference')
        package = page.get_by_label('模型 1 程序包', exact=True)
        package.select_option('demo')
        model = page.get_by_label('模型 1 文件', exact=True)
        model.select_option('/packages/demo/assets/first.rknn')
        page.get_by_label('模型 1 标签文件', exact=True).select_option('/packages/demo/assets/labels.txt')
        page.get_by_label('选择图片', exact=True).set_input_files(dict(name='image.png', mimeType='image/png', buffer=PNG))
        start = page.get_by_role('button', name='开始推理 · 1 个模型', exact=True)
        expect(start).to_be_enabled()
        expect(model.locator('option').last).to_have_text('导入模型…')
        # The actual select action must open the native file chooser.
        def choose(file):
            with page.expect_file_chooser() as chooser:
                model.select_option('__import__')
            chooser.value.set_files(file)
        choose([])
        expect(model).to_have_value('/packages/demo/assets/first.rknn')
        pending = []
        hold = lambda route: pending.append(route)
        page.route('**/api/apps/demo/assets/upload', hold)
        payload = dict(name='first.rknn', mimeType='application/octet-stream', buffer=b'new model')
        choose(payload)
        expect(page.get_by_role('progressbar', name='模型 1 文件导入进度')).to_be_visible()
        expect(package).to_be_disabled()
        expect(start).to_be_disabled()
        assert pending
        pending.pop().continue_()
        page.unroute('**/api/apps/demo/assets/upload', hold)
        expect(model).to_have_value('/packages/demo/assets/first_copy.rknn')
        expect(page.get_by_text('已导入，同名文件已另存', exact=True)).to_be_visible()
        expect(start).to_be_enabled()
        folder = state['package_root'] / 'demo/assets'
        assert (folder / 'first.rknn').read_bytes() == b'original'
        assert (folder / 'first_copy.rknn').read_bytes() == b'new model'
        assert any(check['path'] == '/packages/demo/assets/first_copy.rknn' for check in state['model_checks'])
        # Failure preserves the old selection, and the same file can be retried.
        page.route('**/api/apps/demo/assets/upload', lambda route: route.fulfill(status=500,
                   json={'detail': '保存失败：磁盘空间不足'}), times=1)
        choose(payload)
        expect(page.get_by_role('alert')).to_contain_text('磁盘空间不足')
        expect(model).to_have_value('/packages/demo/assets/first_copy.rknn')
        expect(package).to_be_enabled()
        choose(payload)
        expect(model).to_have_value('/packages/demo/assets/first_copy_2.rknn')
        expect(page.get_by_role('alert')).to_have_count(0)
        choose(dict(name='empty.rknn', mimeType='application/octet-stream', buffer=b''))
        expect(page.get_by_role('alert')).to_contain_text('模型文件为空')
        assert not (folder / 'empty.rknn').exists()
        choose(dict(name='wrong.txt', mimeType='text/plain', buffer=b'wrong'))
        expect(page.get_by_role('alert')).to_contain_text('请选择 .rknn')
        assert not (folder / 'wrong.txt').exists()
        choose(dict(name='broken-new.rknn', mimeType='application/octet-stream', buffer=b'broken'))
        expect(model).to_have_value('/packages/demo/assets/broken-new.rknn')
        expect(page.get_by_role('status', name='模型 1 检查', exact=True)).to_contain_text('无法加载')
        expect(start).to_be_disabled()
        package.select_option('empty')
        choose(dict(name='新模型.rknn', mimeType='application/octet-stream', buffer=b'model'))
        expect(model).to_have_value('/packages/empty/assets/新模型.rknn')
        assert (state['package_root'] / 'empty/assets/新模型.rknn').read_bytes() == b'model'
        assert not (folder / '新模型.rknn').exists()
        page.reload()
        package.select_option('empty')
        expect(model.locator('option')).to_have_text(['选择模型', '新模型.rknn', '导入模型…'])
        assert not errors
        browser.close()


def test_large_batch_upload_and_space_failure_cleanup(console):
    url, state = console
    expect = playwright.expect
    with playwright.sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                                    headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
        page = browser.new_page(viewport={'width': 1366, 'height': 900})
        page.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'test',username:'tester'},version:0}))")
        page.goto(url + '/image-inference')
        page.get_by_label('模型 1 程序包', exact=True).select_option('demo')
        page.get_by_label('模型 1 文件', exact=True).select_option('/packages/demo/assets/first.rknn')
        page.get_by_label('模型 1 标签文件', exact=True).select_option('/packages/demo/assets/labels.txt')
        page.get_by_label('选择图片', exact=True).set_input_files([
            dict(name=f'{index}.png', mimeType='image/png', buffer=PNG) for index in range(101)])
        expect(page.get_by_text('已选 101 张 · 重新选择', exact=True)).to_be_visible()
        # Thumbnail rendering is bounded; every file is still uploaded and inferred.
        expect(page.locator('.image-thumbnails img')).to_have_count(60)
        start = page.get_by_role('button', name='开始推理 · 1 个模型', exact=True)
        start.click()
        expect(page.locator('.image-result-title')).to_contain_text('0.png', timeout=15000)
        assert len(state['uploaded']) == state['job']['total'] == 101
        page.get_by_label('全选检测结果', exact=True).check()
        expect(page.get_by_role('button', name='下载标签 TXT', exact=False)).to_have_text('下载标签 TXT (101)')
        page.route('**/jobs/*/inputs/1', lambda route: route.fulfill(status=507,
                   json={'detail': '存储空间不足：需保留超过 100 MB'}), times=1)
        start.click()
        expect(page.get_by_role('alert')).to_contain_text('100 MB')
        assert state['discarded_upload'] == state['upload_id'] and state['uploaded'] == {}
        expect(start).to_be_enabled()
        assert state['job']['total'] == 101
        browser.close()
