"""Session expiry, readable ANSI backgrounds and continuously animated fine snow."""
import json
import re

import pytest

from test_terminal_display import console, login, click_at, pw_api


@pytest.mark.parametrize('trigger', ['reload', 'disconnect'])
def test_terminal_expired_login_redirects_without_visiting_another_page(console, trigger):
    page, _, _, _ = console
    expired = False
    checks = []
    def auth(route):
        checks.append(route.request.headers.get('authorization'))
        route.fulfill(status=401 if expired else 200, content_type='application/json',
                      body=json.dumps({'detail': 'expired'} if expired else {'username': 'tester'}))
    page.route('**/api/auth/me', auth)
    sockets = []
    page.route_web_socket('**/ws/terminal?**', lambda ws: sockets.append(ws))
    login(page)
    click_at(page, page.get_by_role('link', name=re.compile('终端命令行')))
    page.wait_for_function("document.querySelector('.term-frame-reflection')?.width>300")
    expired = True
    if trigger == 'reload':
        page.reload()
    else:
        sockets[0].close(code=1000, reason='server restarted')
    pw_api.expect(page).to_have_url(re.compile('/login$'))
    pw_api.expect(page.locator('.login-page')).to_be_visible()
    assert page.evaluate("JSON.parse(localStorage.getItem('rk3588-auth')).state.token") is None
    assert len(checks) >= 2 and all(value == 'Bearer isolated' for value in checks)


def test_temporary_disconnect_preserves_login_and_checks_again_on_recovery(console):
    page, _, _, _ = console
    status = 200
    checks = []
    def auth(route):
        checks.append(status)
        route.fulfill(status=status, content_type='application/json', body='{}')
    page.route('**/api/auth/me', auth)
    sockets = []
    page.route_web_socket('**/ws/terminal?**', lambda ws: sockets.append(ws))
    login(page)
    click_at(page, page.get_by_role('link', name=re.compile('终端命令行')))
    page.wait_for_function("document.querySelector('.term-frame-reflection')?.width>300")
    sockets[0].send('KEEP_SCROLLBACK')
    pw_api.expect(page.locator('.xterm-rows')).to_contain_text('KEEP_SCROLLBACK')
    status = 503
    with page.expect_response('**/api/auth/me'):
        sockets[0].close(code=1000)
    pw_api.expect(page.locator('.xterm-rows')).to_contain_text('连接已断开')
    pw_api.expect(page).to_have_url(re.compile('/terminal$'))
    assert page.evaluate("JSON.parse(localStorage.getItem('rk3588-auth')).state.token") == 'isolated'
    status = 401
    page.evaluate("window.dispatchEvent(new Event('online'))")
    pw_api.expect(page).to_have_url(re.compile('/login$'))
    assert 503 in checks and checks[-1] == 401


def test_colored_and_inverse_text_remains_readable_with_glow(console):
    page, _, _, _ = console
    sockets = []
    page.route_web_socket('**/ws/terminal?**', lambda ws: sockets.append(ws))
    login(page)
    click_at(page, page.get_by_role('link', name=re.compile('终端命令行')))
    page.wait_for_function("window.terminalFontLoaded('VT323',20)")
    page.get_by_label('终端字体', exact=True).select_option('font:Departure Mono')
    page.get_by_role('spinbutton', name='字号', exact=True).fill('28')
    page.wait_for_function("window.terminalFontLoaded('Departure Mono',28)")
    click_at(page, page.get_by_role('button', name='荧光绿', exact=True))
    click_at(page, page.get_by_role('button', name=re.compile('显示调校')))
    for name in ['荧光辉光', '光晕扩散']:
        page.get_by_role('slider', name=re.compile(name)).fill('100')
    click_at(page, page.get_by_role('button', name=re.compile('显示调校')))
    page.emulate_media(reduced_motion='reduce')
    sockets[0].send('\x1b[?25l\x1b[2J\x1b[H'
                    'root@machine:/userdata# ls\r\n'
                    '\x1b[37;46m rk3588_visual_analysis_framework \x1b[0m\r\n'
                    '\x1b[7m INVERSE_DIRECTORY \x1b[0m\r\n'
                    '\x1b[38;2;160;200;175;48;2;165;205;180m TRUECOLOR_DIRECTORY \x1b[0m\r\n'
                    '\x1b[38;5;15;48;5;14m INDEXED_DIRECTORY \x1b[0m\r\n'
                    '\x1b[32mNORMAL_GREEN\x1b[0m')
    pw_api.expect(page.locator('.xterm-rows')).to_contain_text('NORMAL_GREEN')
    for text in ['rk3588_visual_analysis_framework', 'INVERSE_DIRECTORY', 'TRUECOLOR_DIRECTORY', 'INDEXED_DIRECTORY']:
        span = page.locator('.xterm-rows > div').filter(has_text=text).locator('span').filter(has_text=re.compile(r'\S')).first
        result = span.evaluate(r'''e => {
            const s=getComputedStyle(e), rgb=c=>c.match(/[\d.]+/g).slice(0,3).map(Number);
            const lum=c=>rgb(c).reduce((sum,v,i)=>{v/=255;return sum+(v<=.04045?v/12.92:((v+.055)/1.055)**2.4)*[.2126,.7152,.0722][i]},0);
            const a=lum(s.color),b=lum(s.backgroundColor);
            return {contrast:(Math.max(a,b)+.05)/(Math.min(a,b)+.05),shadow:s.textShadow};
        }''')
        assert result['contrast'] >= 4.5, (text, result)
        assert result['shadow'] == 'none', (text, result)
    assert page.locator('.xterm-rows > div').filter(has_text='NORMAL_GREEN').locator('span').first.evaluate('e=>getComputedStyle(e).textShadow') != 'none'
    page.locator('.term-screen').screenshot(path='/tmp/terminal-crt-readable-colors.png')


def test_fine_snow_is_bipolar_cached_and_continuously_animated(console):
    page, _, _, _ = console
    login(page)
    click_at(page, page.get_by_role('link', name=re.compile('终端命令行')))
    page.wait_for_function("document.querySelector('.term-frame-reflection')?.width>300")
    noise = page.locator('.term-crt-noise')
    result = noise.evaluate('''async e => {
        const url=getComputedStyle(e).backgroundImage.slice(5,-2), image=new Image();
        image.src=url; await image.decode();
        const c=document.createElement('canvas');c.width=image.width;c.height=image.height;
        const ctx=c.getContext('2d');ctx.drawImage(image,0,0);
        const pixels=ctx.getImageData(0,0,c.width,c.height).data;
        let dark=0,light=0,faint=0,strong=0;
        for(let i=0;i<pixels.length;i+=4){
            if(pixels[i+3]>0){if(pixels[i]===0)dark++;else light++;}
            if(pixels[i+3]<96)faint++;else strong++;
        }
        return {dark,light,faint,strong,width:image.width,url};
    }''')
    assert result['dark'] > 10000 and result['light'] > 10000
    assert result['faint'] > result['strong'] * 3
    assert result['width'] == 512
    page.wait_for_timeout(4000)
    frames = noise.evaluate('''async e=>{
        const values=[];for(let i=0;i<8;i++){
            values.push(getComputedStyle(e).transform);await new Promise(r=>setTimeout(r,110));
        }return values;
    }''')
    assert len(set(frames)) >= 4
    # Texture generation is not repeated on the animation clock.
    assert noise.evaluate('e=>getComputedStyle(e).backgroundImage.slice(5,-2)') == result['url']
    page.locator('.term-screen').screenshot(path='/tmp/terminal-crt-fine-snow.png')
    click_at(page, page.get_by_role('button', name=re.compile('显示调校')))
    page.get_by_role('slider', name='雪花质感', exact=True).fill('0')
    pw_api.expect(noise).to_have_css('opacity', '0')
    pw_api.expect(noise).to_have_css('animation-name', 'none')
