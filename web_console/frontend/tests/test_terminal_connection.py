"""Managed transport: reconnect, committed output offsets and explicit endings."""
import json
import re
import struct
from urllib.parse import parse_qs, urlsplit

import pytest

from test_terminal_display import console, login, click_at, pw_api


def start(page):
    page.evaluate("localStorage.setItem('terminal-appearance-v1',JSON.stringify({enabled:false}))")
    login(page)
    click_at(page, page.get_by_role('link', name=re.compile('终端命令行')))
    pw_api.expect(page.locator('.term-connection')).to_have_text('已连接')


def frame(text, start=0):
    data = text.encode()
    return struct.pack('!Q', start+len(data)) + data


def test_reconnect_reuses_session_and_deduplicates_output_without_replaying_input(console):
    page, _, _, _ = console
    sockets, queries, controls, inputs = [], [], [], []
    first = 'ONCE_ONLY\r\n'
    second = 'AFTER_RECOVERY\r\n'
    def connected(ws):
        sockets.append(ws)
        query = parse_qs(urlsplit(ws.url).query)
        queries.append(query)
        def message(value):
            if isinstance(value, bytes):
                controls.append(json.loads(value))
            else:
                inputs.append(value)
        ws.on_message(message)
        ws.send(json.dumps({'type': 'terminal.ready', 'resumed': len(sockets)>1}))
        # Deliberately resend the already rendered range to exercise de-duplication.
        ws.send(frame(first))
        if len(sockets)>1:
            ws.send(frame(second, len(first.encode())))
        ws.send(json.dumps({'type': 'terminal.ping'}))
    page.route_web_socket('**/ws/terminal?**', connected)
    start(page)
    pw_api.expect(page.locator('.xterm-rows')).to_contain_text('ONCE_ONLY')
    page.locator('.xterm-helper-textarea').focus()
    page.keyboard.type('execute_once')
    for _ in range(50):
        if any(c['type']=='ack' and c['offset']==len(first.encode()) for c in controls):
            break
        page.wait_for_timeout(50)
    assert any(c['type']=='pong' for c in controls)
    assert any(c['type']=='ack' and c['offset']==len(first.encode()) for c in controls)
    sockets[0].close(code=1011, reason='simulated network failure')
    pw_api.expect(page.locator('.xterm-rows')).to_contain_text('AFTER_RECOVERY', timeout=10000)
    pw_api.expect(page.locator('.term-connection')).to_have_text('已连接')
    text = page.locator('.xterm-rows').inner_text()
    assert text.count('ONCE_ONLY') == 1 and '连接已断开' not in text
    assert len(sockets) == 2 and queries[0]['sid'] == queries[1]['sid']
    assert queries[1]['resume'] == ['1'] and queries[1]['offset'] == [str(len(first.encode()))]
    assert ''.join(v for v in inputs if not v.startswith('\x1b[resize:')) == 'execute_once'
    click_at(page, page.get_by_role('button', name=re.compile('新建分屏')))
    pw_api.expect(page.locator('.term-pane')).to_have_count(2)
    click_at(page, page.get_by_role('button', name='关闭终端 1', exact=True))
    page.wait_for_timeout(1000)
    assert any(c['type']=='close' for c in controls)
    assert len(sockets) == 3


@pytest.mark.parametrize('ending', ['exit', 'expired'])
def test_ended_session_does_not_restart_the_shell(console, ending):
    page, _, _, _ = console
    sockets = []
    def connected(ws):
        sockets.append(ws)
        ws.send(json.dumps({'type':'terminal.ready'}))
    page.route_web_socket('**/ws/terminal?**', connected)
    start(page)
    if ending == 'exit':
        sockets[0].send(json.dumps({'type':'terminal.exit','exit_code':0}))
        sockets[0].close(code=1000)
        pw_api.expect(page.locator('.term-connection')).to_have_text('进程已退出')
    else:
        sockets[0].close(code=4004, reason='resume retention elapsed')
        pw_api.expect(page.locator('.term-connection')).to_have_text('会话已结束')
    page.evaluate("window.dispatchEvent(new Event('online'))")
    page.wait_for_timeout(1800)
    assert len(sockets) == 1


def test_returning_to_a_stale_page_replaces_the_transport(console):
    page, _, _, _ = console
    sockets, queries = [], []
    def connected(ws):
        sockets.append(ws)
        queries.append(parse_qs(urlsplit(ws.url).query))
        ws.send(json.dumps({'type':'terminal.ready'}))
    page.route_web_socket('**/ws/terminal?**', connected)
    start(page)
    page.evaluate('''() => {
        const now=Date.now;Date.now=()=>now()+65000;
        document.dispatchEvent(new Event('visibilitychange'));
    }''')
    for _ in range(50):
        if len(sockets) == 2:
            break
        page.wait_for_timeout(50)
    assert len(sockets) == 2 and queries[0]['sid'] == queries[1]['sid']
    assert queries[1]['resume'] == ['1']
    pw_api.expect(page.locator('.term-connection')).to_have_text('已连接')


def test_logout_cleans_up_lazy_terminal_and_next_login_opens_new_session(console):
    page, _, _, _ = console
    queries, controls = [], []

    def connected(ws):
        queries.append(parse_qs(urlsplit(ws.url).query))
        ws.on_message(lambda value: controls.append(json.loads(value)) if isinstance(value, bytes) else None)
        ws.send(json.dumps({'type': 'terminal.ready'}))

    page.route_web_socket('**/ws/terminal?**', connected)
    start(page)
    click_at(page, page.get_by_role('button', name='退出登录', exact=True))
    pw_api.expect(page.locator('.login-page')).to_be_visible()
    for _ in range(50):
        if any(control['type'] == 'close' for control in controls):
            break
        page.wait_for_timeout(50)
    assert any(control['type'] == 'close' for control in controls)
    page.evaluate("window.dispatchEvent(new Event('online'))")
    page.wait_for_timeout(1000)
    assert len(queries) == 1
    start(page)
    assert len(queries) == 2
    assert queries[0]['sid'] != queries[1]['sid']
