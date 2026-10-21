"""Browser regressions for live log following, retention and history scrolling."""

from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import re
from pathlib import Path
import threading
from urllib.parse import urlsplit

import pytest

pw_api = pytest.importorskip('playwright.sync_api')
DIST = Path(__file__).resolve().parents[1] / 'dist'


@pytest.fixture
def console():
    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(DIST), **kwargs)

        def do_GET(self):
            if not self.path.startswith('/assets/'):
                self.path = '/index.html'
            super().do_GET()

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        with pw_api.sync_playwright() as pw:
            browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
            context = browser.new_context(viewport={'width': 1500, 'height': 1000})
            context.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'isolated',username:'tester'},version:0}))")
            # Playwright's WebSocket interception uses URL.parse, which older device Chromium lacks.
            context.add_init_script("URL.parse ??= (url, base) => { try { return new URL(url, base) } catch { return null } }")
            page = context.new_page()
            errors, sockets = [], []
            page.on('pageerror', lambda error: errors.append(str(error)))

            def api(route):
                path = urlsplit(route.request.url).path
                responses = {
                    '/api/apps': [dict(name='demo', status='running', pid=123, active_config='config.json')],
                    '/api/apps/demo/config': {'global': {'enable_rtsp': 0}, 'channels': []},
                    '/api/apps/demo/logic-actions': {'globals': [], 'channels': [], 'socket_ready': True},
                }
                assert route.request.method == 'GET'
                assert path in responses, path
                route.fulfill(content_type='application/json', body=json.dumps(responses[path]))

            page.route('**/api/**', api)
            page.route_web_socket('**/ws/logs/**', lambda socket: sockets.append(socket))
            page.goto(f'http://127.0.0.1:{server.server_port}/live-view')
            pw_api.expect(page.locator('.live-view-log-status')).to_have_text('● 实时')
            yield page, sockets
            assert not errors, errors
            browser.close()
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=5)


def send_lines(console, start, count, width=0):
    page, sockets = console
    sockets[-1].send('\n'.join(f'line-{number:05d} ' + 'x' * width for number in range(start, start + count)) + '\n')
    pw_api.expect(page.locator('.live-view-log-line').last).to_contain_text(f'line-{start + count - 1:05d}')


def expect_bottom(page):
    page.wait_for_function('''() => {
        const e = document.querySelector('.live-view-console');
        return e.scrollHeight - e.scrollTop - e.clientHeight < 2;
    }''', timeout=3000)


def test_follow_continues_after_retention_limit_with_wrapped_lines(console):
    page, _ = console
    send_lines(console, 0, 1000)
    expect_bottom(page)
    for start, width in ((1000, 500), (1120, 0), (1240, 1000), (1360, 0)):
        send_lines(console, start, 120, width)
        expect_bottom(page)
        pw_api.expect(page.locator('.live-view-log-line')).to_have_count(1000)
    send_lines(console, 1480, 1)
    expect_bottom(page)


@pytest.mark.parametrize('resume', ['button', 'scroll'])
def test_history_stays_still_and_resume_flushes_pending_logs(console, resume):
    page, sockets = console
    send_lines(console, 0, 1000)
    expect_bottom(page)
    box = page.locator('.live-view-console')
    box.hover()
    page.mouse.wheel(0, -600)
    page.wait_for_function('''() => {
        const e = document.querySelector('.live-view-console');
        return e.scrollHeight - e.scrollTop - e.clientHeight > 100;
    }''')
    pw_api.expect(page.locator('.live-view-log-status')).to_have_text('Ⅱ 已暂停跟随')
    # Native wheel scrolling can continue animating after the first scroll event.
    box.evaluate('''e => new Promise(resolve => {
        let previous = e.scrollTop, stable = 0;
        const measure = () => {
            stable = Math.abs(e.scrollTop - previous) < 0.5 ? stable + 1 : 0;
            previous = e.scrollTop;
            if (stable >= 4) resolve(); else requestAnimationFrame(measure);
        };
        requestAnimationFrame(measure);
    })''')
    before = box.evaluate('e => [e.scrollTop, e.textContent]')
    sockets[-1].send('pending-first\npending-second\n')
    page.wait_for_timeout(100)
    assert box.evaluate('e => [e.scrollTop, e.textContent]') == before
    if resume == 'button':
        page.get_by_role('button', name=re.compile('^跳到底部')).click()
    else:
        box.evaluate('e => { e.scrollTop = e.scrollHeight }')
    pw_api.expect(page.locator('.live-view-log-line').last).to_have_text('pending-second')
    pw_api.expect(page.locator('.live-view-log-status')).to_have_text('● 实时')
    expect_bottom(page)
    send_lines(console, 1002, 1)
    expect_bottom(page)
    # The jump button must work even when no new lines arrived while paused.
    box.hover()
    page.mouse.wheel(0, -600)
    pw_api.expect(page.locator('.live-view-log-status')).to_have_text('Ⅱ 已暂停跟随')
    page.get_by_role('button', name=re.compile('^跳到底部')).click()
    expect_bottom(page)
    pw_api.expect(page.locator('.live-view-log-status')).to_have_text('● 实时')


def test_follow_survives_console_resize(console):
    page, _ = console
    send_lines(console, 0, 1000, 250)
    expect_bottom(page)
    page.set_viewport_size({'width': 1000, 'height': 1000})
    page.wait_for_timeout(200)
    send_lines(console, 1000, 1)
    expect_bottom(page)


def test_follow_survives_rapid_messages(console):
    page, sockets = console
    session = page.context.new_cdp_session(page)
    session.send('Emulation.setCPUThrottlingRate', {'rate': 8})
    send_lines(console, 0, 1000, 100)
    expect_bottom(page)
    page.evaluate('''() => {
        window.logTextChanges = 0;
        new MutationObserver(records => {
            window.logTextChanges += records.filter(r => r.type === 'characterData').length;
        }).observe(document.querySelector('.live-view-console'), {subtree:true, characterData:true});
    }''')
    for number in range(1000, 1150):
        sockets[-1].send('\n'.join(f'rapid-{number}-{i} ' + 'x' * (number % 7 * 100) for i in range(20)) + '\n')
        page.wait_for_timeout(2)
    sockets[-1].send('rapid-finished\n')
    pw_api.expect(page.locator('.live-view-log-line').last).to_have_text('rapid-finished')
    expect_bottom(page)
    # Existing rows must stay unchanged when old rows are evicted; rewriting 1000
    # text nodes per incoming frame monopolizes the thread shared with MSE playback.
    assert page.evaluate('window.logTextChanges') < 4000


def test_paused_backlog_is_bounded_and_resume_shows_latest_output(console):
    page, sockets = console
    send_lines(console, 0, 1000)
    page.locator('.live-view-console').hover()
    page.mouse.wheel(0, -600)
    pw_api.expect(page.locator('.live-view-log-status')).to_have_text('Ⅱ 已暂停跟随')
    sockets[-1].send('\n'.join(f'backlog-{i}' for i in range(6000)) + '\n')
    button = page.get_by_role('button', name='跳到底部（5000+）', exact=True)
    pw_api.expect(button).to_be_visible()
    pw_api.expect(page.locator('.live-view-log-line').last).to_contain_text('line-00999')
    button.click()
    pw_api.expect(page.locator('.live-view-log-line').last).to_have_text('backlog-5999')
    pw_api.expect(page.locator('.live-view-log-line')).to_have_count(1000)
    expect_bottom(page)


def test_log_socket_reconnects_after_close(console):
    page, sockets = console
    send_lines(console, 0, 2)
    sockets[-1].close()
    pw_api.expect(page.locator('.live-view-log-status')).to_have_text('○ 重连中')
    pw_api.expect(page.locator('.live-view-log-status')).to_have_text('● 实时', timeout=4000)
    assert len(sockets) == 2
    send_lines(console, 2, 2)
