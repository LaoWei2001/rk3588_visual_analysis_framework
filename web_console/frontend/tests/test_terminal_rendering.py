"""Visible CRTs keep animating without output; hidden displays can pause."""
import re

from test_terminal_display import console, login, click_at, pw_api


def test_idle_terminal_keeps_animating_until_hidden(console):
    page, _, _, messages = console
    sockets = []
    def terminal(ws):
        sockets.append(ws)
        ws.on_message(lambda message: messages.append(message))
    page.route_web_socket('**/ws/terminal?**', terminal)
    login(page)
    click_at(page, page.get_by_role('link', name=re.compile('终端命令行')))
    page.wait_for_function("document.querySelector('.term-frame-reflection')?.width > 300")
    sockets[0].send('\x1b[2J\x1b[HIDLE_CHECK')
    pw_api.expect(page.locator('.xterm-rows')).to_contain_text('IDLE_CHECK')
    page.wait_for_timeout(4000)
    metrics = page.evaluate('''async () => {
        let draws = 0;
        const proto = CanvasRenderingContext2D.prototype, original = proto.drawImage;
        proto.drawImage = function(...args) { draws++; return original.apply(this, args); };
        const content = document.querySelector('.term-phosphor-content');
        const before = content.dataset.signalFrame;
        await new Promise(resolve => setTimeout(resolve, 1200));
        proto.drawImage = original;
        return {draws, signalChanged: before !== content.dataset.signalFrame,
            running: document.querySelector('.term-screen').getAnimations({subtree:true})
                .filter(a => a.playState === 'running').length};
    }''')
    print('idle CRT work over 1.2s:', metrics)
    assert metrics['draws'] > 0 and metrics['signalChanged'] and metrics['running'] >= 4
    pw_api.expect(page.locator('.term-screen')).to_have_attribute('data-effects-paused', 'false')
    assert page.locator('.term-crt-surface').evaluate("e=>getComputedStyle(e).filter") != 'none'
    sockets[0].send('\r\nOUTPUT_WAKES')
    pw_api.expect(page.locator('.xterm-rows')).to_contain_text('OUTPUT_WAKES')
    pw_api.expect(page.locator('.term-screen')).to_have_attribute('data-effects-paused', 'false')
    page.keyboard.type('echo awake')
    pw_api.expect(page.locator('.term-screen')).to_have_attribute('data-effects-paused', 'false')
    assert 'echo awake' in ''.join(messages)
    # Background output updates the buffer without restarting invisible effects.
    page.evaluate("Object.defineProperty(document,'hidden',{configurable:true,get:()=>true});document.dispatchEvent(new Event('visibilitychange'))")
    sockets[0].send('\r\nBACKGROUND_OUTPUT')
    pw_api.expect(page.locator('.xterm-rows')).to_contain_text('BACKGROUND_OUTPUT')
    pw_api.expect(page.locator('.term-screen')).to_have_attribute('data-effects-paused', 'true')
    page.evaluate("delete document.hidden;document.dispatchEvent(new Event('visibilitychange'))")
    pw_api.expect(page.locator('.term-screen')).to_have_attribute('data-effects-paused', 'false')
    page.emulate_media(reduced_motion='reduce')
    pw_api.expect(page.locator('.term-screen')).to_have_attribute('data-effects-paused', 'true')
    assert len(sockets) == 1


def test_split_terminals_keep_animating_after_navigation(console):
    page, _, _, _ = console
    sockets = []
    page.route_web_socket('**/ws/terminal?**', lambda ws: sockets.append(ws))
    login(page)
    click_at(page, page.get_by_role('link', name=re.compile('终端命令行')))
    click_at(page, page.get_by_role('button', name=re.compile('新建分屏')))
    pw_api.expect(page.locator('.term-pane')).to_have_count(2)
    page.wait_for_timeout(4000)
    sockets[1].send('\r\nSECOND_PANE_OUTPUT')
    pw_api.expect(page.locator('.xterm-rows').nth(1)).to_contain_text('SECOND_PANE_OUTPUT')
    pw_api.expect(page.locator('.term-screen').nth(0)).to_have_attribute('data-effects-paused', 'false')
    pw_api.expect(page.locator('.term-screen').nth(1)).to_have_attribute('data-effects-paused', 'false')
    # Route changes detach the display, preserving both shell sessions and output.
    click_at(page, page.get_by_role('link', name=re.compile('程序管理')))
    pw_api.expect(page.locator('.term-pane')).to_have_count(0)
    click_at(page, page.get_by_role('link', name=re.compile('终端命令行')))
    pw_api.expect(page.locator('.xterm-rows').nth(1)).to_contain_text('SECOND_PANE_OUTPUT')
    page.wait_for_timeout(4000)
    for screen in page.locator('.term-screen').all():
        pw_api.expect(screen).to_have_attribute('data-effects-paused', 'false')
        assert screen.evaluate("e=>e.getAnimations({subtree:true}).some(a=>a.playState==='running')")
    assert len(sockets) == 2


def test_disabled_reflection_does_not_allocate_webgl(console):
    page, _, _, _ = console
    page.evaluate('''() => {
        const prefs=JSON.parse(localStorage.getItem('terminal-appearance-v1'));
        prefs.reflection=0; prefs.afterglow=0;
        localStorage.setItem('terminal-appearance-v1', JSON.stringify(prefs));
        window.webglContexts=0;
        const original=HTMLCanvasElement.prototype.getContext;
        HTMLCanvasElement.prototype.getContext=function(kind,...args) {
            if (kind==='webgl' || kind==='webgl2') window.webglContexts++;
            return original.call(this,kind,...args);
        };
    }''')
    login(page)
    click_at(page, page.get_by_role('link', name=re.compile('终端命令行')))
    pw_api.expect(page.locator('.xterm-rows')).to_contain_text('EDGE_SELECT')
    page.wait_for_timeout(300)
    assert page.evaluate('window.webglContexts') == 0
    # Re-enabling afterglow alone should still leave the reflection GPU unused.
    click_at(page, page.get_by_role('button', name=re.compile('显示调校')))
    page.get_by_role('slider', name='荧光残影', exact=True).fill('55')
    page.wait_for_function("document.querySelector('.term-crt-afterglow').width>300")
    assert page.evaluate('window.webglContexts') == 0
