"""告警发件箱筛选的浏览器回归：构建前端后，使用真实 Records HTTP 路由。"""

import json
import os
from pathlib import Path
import socket
import threading
import time

import pytest
import uvicorn
from fastapi import FastAPI
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from routers import records

playwright = pytest.importorskip('playwright.sync_api')
FRONTEND = Path(__file__).resolve().parents[1]


def write_record(store, event_id, event_type, timestamp, media=()):
    path = store / event_id
    path.mkdir()
    milliseconds = int(timestamp * 1000)
    (path / 'event.json').write_text(json.dumps({
        'event': {'id': event_id, 'type': event_type, 'trigger_unix_ms': milliseconds,
                  'created_unix_sec': timestamp, 'snap_time': milliseconds},
        'source': {'channel_id': 0}, 'data': {'fields': {}},
    }), encoding='utf-8')
    (path / 'media_state.json').write_text(json.dumps({'status': 'ready', 'media': {}}))
    (path / 'delivery_state.json').write_text(json.dumps({
        'deliveries': [{'id': 'delivery', 'media': list(media)}] if media else [],
    }))


@pytest.fixture
def console(tmp_path, monkeypatch):
    if not (FRONTEND / 'dist/index.html').is_file():
        pytest.skip('Build the frontend first')
    store = tmp_path / 'event_store'
    store.mkdir()
    monkeypatch.setenv('EVENT_STORE_DIR', str(store))
    app = FastAPI()
    app.include_router(records.router, prefix='/api')
    app.mount('/assets', StaticFiles(directory=FRONTEND / 'dist/assets'), name='assets')

    @app.get('/{path:path}')
    async def frontend(path: str):
        return FileResponse(FRONTEND / 'dist/index.html')

    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    port = listener.getsockname()[1]
    server = uvicorn.Server(uvicorn.Config(app, log_level='error', lifespan='off'))
    worker = threading.Thread(target=lambda: server.run(sockets=[listener]), daemon=True)
    worker.start()
    for _ in range(100):
        if server.started:
            break
        time.sleep(.05)
    assert server.started
    try:
        yield f'http://127.0.0.1:{port}', store
    finally:
        server.should_exit = True
        worker.join(timeout=5)
        listener.close()


def test_event_type_time_and_media_filters_and_live_type_options(console):
    url, store = console
    # UTC timestamps correspond to local noon on Oct 1 and Oct 2 in Shanghai.
    write_record(store, 'hook-old', 'crane_hook_outside', 1790827200)
    write_record(store, 'hook-new', 'crane_hook_outside', 1790913600, ('video',))
    write_record(store, 'helmet-new', 'crane_still_no_helmet', 1790917200)
    expect = playwright.expect
    with playwright.sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                                     headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
        context = browser.new_context(timezone_id='Asia/Shanghai')
        context.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'test',username:'tester'},version:0}))")
        page = context.new_page()
        errors = []
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.goto(url + '/records/demo')
        types = page.get_by_label('事件类型', exact=True)
        cards = page.locator('.rec-card')
        expect(cards).to_have_count(3)
        expect(types.locator('option')).to_have_text(['全部类型', 'crane_hook_outside', 'crane_still_no_helmet'])

        types.select_option('crane_hook_outside')
        expect(cards).to_have_count(2)
        page.get_by_label('选择记录 hook-old', exact=True).check()
        page.get_by_label('开始', exact=True).fill('2026-10-02T00:00')
        expect(cards).to_have_count(1)
        expect(page.get_by_label('选择记录 hook-new', exact=True)).not_to_be_checked()
        expect(page.locator('.records-filter-summary')).to_have_text('显示 1 / 1 条')
        expect(types.locator('option')).to_have_count(3)

        page.get_by_role('button', name='仅数据', exact=True).click()
        expect(page.get_by_text('当前筛选条件下没有告警记录。', exact=True)).to_be_visible()
        types.select_option('crane_still_no_helmet')
        expect(cards).to_have_count(1)
        expect(cards).to_contain_text('crane_still_no_helmet')
        expect(page.get_by_label('开始', exact=True)).to_have_value('2026-10-02T00:00')

        types.select_option('crane_hook_outside')
        page.get_by_role('button', name='清除时间', exact=True).click()
        expect(cards).to_have_count(1)
        expect(cards).to_contain_text('crane_hook_outside')
        expect(types).to_have_value('crane_hook_outside')
        page.get_by_role('button', name='全部', exact=True).click()
        expect(cards).to_have_count(2)
        for width in (1366, 390):
            page.set_viewport_size({'width': width, 'height': 1100})
            expect(types).to_be_visible()
            size = page.locator('.records-toolbar').evaluate('(e) => [e.clientWidth, e.scrollWidth]')
            assert size[1] <= size[0] + 1, (width, size)

        # Removing the last records of the selected type refreshes the catalog
        # and returns to all types; newly arriving custom types are selectable.
        assert page.request.delete(url + '/api/apps/demo/records/hook-old').status == 200
        assert page.request.delete(url + '/api/apps/demo/records/hook-new').status == 200
        page.get_by_role('button', name='↻ 刷新', exact=True).click()
        expect(types).to_have_value('')
        expect(types.locator('option')).to_have_text(['全部类型', 'crane_still_no_helmet'])
        expect(cards).to_have_count(1)
        write_record(store, 'custom-new', '自定义/事件 & A', 1790917300)
        page.get_by_role('button', name='↻ 刷新', exact=True).click()
        expect(types.locator('option')).to_have_count(3)
        types.select_option('自定义/事件 & A')
        expect(cards).to_have_count(1)
        expect(cards).to_contain_text('自定义/事件 & A')
        assert not errors, errors
        browser.close()
