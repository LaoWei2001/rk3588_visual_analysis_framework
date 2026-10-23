"""Keep CRT styling inside the terminal while retaining the original console UI."""
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import threading
from urllib.parse import urlsplit

import pytest

pw_api = pytest.importorskip('playwright.sync_api')
DIST = Path(__file__).resolve().parents[1] / 'dist'


def screen_point(locator, fx=.5, fy=.5):
    return locator.evaluate('''(e,{fx,fy})=>{
        const r=e.getBoundingClientRect();let px=r.left+r.width*fx,py=r.top+r.height*fy;
        const surface=e.closest('.term-crt-surface');
        if(surface && getComputedStyle(surface).filter!=='none') {
            const rect=surface.getBoundingClientRect();
            const scale=Number(surface.parentElement.querySelector('.term-curvature-defs feDisplacementMap').getAttribute('scale'));
            const amount=scale/Math.max(rect.width,rect.height);
            const sx=(px-rect.left)/rect.width*2-1,sy=(py-rect.top)/rect.height*2-1;
            let x=sx,y=sy;
            for(let i=0;i<25;i++){x=sx/(1+amount*y*y);y=sy/(1+amount*x*x);}
            px=rect.left+(x+1)*rect.width/2;py=rect.top+(y+1)*rect.height/2;
        }
        return {x:px,y:py};
    }''',dict(fx=fx,fy=fy))


def click_at(page, locator, fx=.5, fy=.5):
    locator.wait_for(state='visible')
    page.mouse.click(**screen_point(locator,fx,fy))


def drag_at(page, start, end):
    # Use CDP input directly: recent Playwright's automatic HTML drag interception
    # is incompatible with the device's Chromium 91. These are trusted browser events.
    cdp = page.context.new_cdp_session(page)
    cdp.send('Input.dispatchMouseEvent', dict(type='mouseMoved', **start))
    cdp.send('Input.dispatchMouseEvent', dict(type='mousePressed', **start, button='left', buttons=1, clickCount=1))
    for step in range(1, 11):
        cdp.send('Input.dispatchMouseEvent', dict(type='mouseMoved',
            x=start['x']+(end['x']-start['x'])*step/10, y=start['y']+(end['y']-start['y'])*step/10,
            button='left', buttons=1))
        page.wait_for_timeout(20)
    # Native HTML dragging needs a separate dragover after dragenter on Chromium 91.
    cdp.send('Input.dispatchMouseEvent', dict(type='mouseMoved', **end, button='left', buttons=1))
    cdp.send('Input.dispatchMouseEvent', dict(type='mouseReleased', **end, button='left', buttons=0, clickCount=1))
    cdp.detach()


@pytest.fixture
def console():
    if not (DIST / 'index.html').is_file():
        pytest.skip('Build the frontend first')
    app = dict(name='demo', status='stopped', pid=None, uptime_seconds=None, config_files=['config.json','yard.json'],
        active_config='config.json', models=[], labels=[], videos=[], has_binary=True, has_config=True,
        autostart=False, unreported=0)
    writes, errors, terminal_messages = [], [], []

    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(DIST), **kwargs)
        def do_GET(self):
            if not self.path.startswith('/assets/'):
                self.path='/index.html'
            super().do_GET()
        def log_message(self, *_args):
            pass

    server=ThreadingHTTPServer(('127.0.0.1',0), Handler)
    worker=threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        with pw_api.sync_playwright() as pw:
            browser=pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH','/usr/bin/chromium'),
                headless=os.environ.get('CHROMIUM_HEADED') != '1', args=['--no-sandbox','--disable-dev-shm-usage','--no-proxy-server'])
            context=browser.new_context(viewport={'width':1500,'height':1000})
            context.add_init_script('''
                if (!localStorage.getItem('terminal-appearance-v1'))
                    localStorage.setItem('terminal-appearance-v1', JSON.stringify({profile:'amber',enabled:true,font:'vt323',fontPreset:1}));
                localStorage.setItem('console-display-v1', JSON.stringify({profile:'amber',glow:100,noise:100,scanlines:100,curvature:100}));
                URL.parse ??= (url,base)=>{try{return new URL(url,base)}catch{return null}};
                window.terminalFontLoaded=(label,size=20)=>{
                    const rows=document.querySelector('.xterm-rows');if(!rows)return false;
                    const family=getComputedStyle(rows).fontFamily;
                    return family.includes(label)&&document.fonts.check(size+'px '+family.split(',')[0]);
                };
                Object.defineProperty(navigator,'clipboard',{value:{writeText:async text=>{window.copiedTerminalText=text}}});
            ''')
            page=context.new_page()
            page.on('pageerror',lambda error:errors.append(str(error)))

            def api(route):
                path=urlsplit(route.request.url).path
                if route.request.method != 'GET':
                    writes.append((path,route.request.post_data_json))
                if path=='/api/auth/login': body=dict(token='isolated',username='tester')
                elif path=='/api/apps': body=[app]
                elif path.endswith('/autostart'):
                    app['autostart']=route.request.post_data_json['enabled'];body=dict(ok=True)
                elif path.endswith('/config') or path.endswith('/config-file'): body={'global':{},'channels':[]}
                elif path.endswith('/config-files'): body=['assets/config.json','assets/yard.json']
                elif path.endswith('/assets'): body=dict(models=[],labels=[],videos=[])
                elif path.endswith('/connections'): body=dict(connections={})
                elif path.endswith('/logics'): body=dict(channel_logics=[],global_logics=[],model_types=[])
                elif path=='/api/console/info': body=dict(version='test',apps_root='/isolated',binary_name='vision_analysis',known_model_types=[])
                else: body={}
                route.fulfill(content_type='application/json',body=json.dumps(body))

            page.route(re.compile(r'^http://[^/]+/api/'),api)
            def terminal(ws):
                ws.on_message(lambda message:terminal_messages.append(message))
                ws.send('\x1b[2J\x1b[HEDGE_SELECT first row\r\nroot@rk3588:~# ')
            page.route_web_socket('**/ws/terminal?**',terminal)
            url=f'http://127.0.0.1:{server.server_port}'
            page.goto(url+'/login')
            pw_api.expect(page.locator('.login-page')).to_be_visible()
            yield page,url,writes,terminal_messages
            assert not errors,errors
            browser.close()
    finally:
        server.shutdown();server.server_close();worker.join(timeout=5)


def login(page):
    click_at(page,page.locator('.login-field input').nth(0))
    page.keyboard.type('tester')
    click_at(page,page.locator('.login-field input').nth(1))
    page.keyboard.type('secret')
    click_at(page,page.get_by_role('button',name=re.compile('登.*录')))
    pw_api.expect(page.locator('.app-card')).to_have_count(1)


def test_original_design_ignores_old_global_retro_preferences(console):
    page,_,_,_=console
    pw_api.expect(page.locator('.console-display,.console-screen-effects')).to_have_count(0)
    pw_api.expect(page.get_by_label('整站显示风格',exact=True)).to_have_count(0)
    assert page.evaluate("getComputedStyle(document.documentElement).getPropertyValue('--accent').trim()")=='#4f8ef7'
    pw_api.expect(page.locator('.login-title')).to_have_css('text-shadow','none')
    login(page)
    pw_api.expect(page.locator('.sidebar')).to_have_css('width','180px')
    pw_api.expect(page.locator('.term-crt-overlay')).to_have_count(0)
    page.reload()
    assert page.evaluate("getComputedStyle(document.documentElement).getPropertyValue('--accent').trim()")=='#4f8ef7'
    pw_api.expect(page.locator('.console-screen-noise,.console-screen-scanlines')).to_have_count(0)


def test_flat_native_controls_modal_and_scroll(console):
    page,_,writes,_=console
    login(page)
    with page.expect_file_chooser() as chooser:
        click_at(page,page.get_by_role('button',name='上传程序',exact=True))
    chooser.value.set_files([])
    # Original design uses a visible switch label around a hidden checkbox.
    click_at(page,page.locator('.autostart-toggle'))
    pw_api.expect(page.locator('.autostart-toggle input')).to_be_checked()
    assert ('/api/apps/demo/autostart',{'enabled':True}) in writes
    page.locator('.config-row select').select_option('yard.json')
    pw_api.expect(page.locator('.config-row select')).to_have_value('yard.json')
    click_at(page,page.get_by_role('button',name='删除',exact=True))
    pw_api.expect(page.get_by_role('dialog')).to_be_visible()
    click_at(page,page.get_by_role('button',name='取消',exact=True))
    pw_api.expect(page.get_by_role('dialog')).to_have_count(0)
    cards=[dict(name=f'demo-{index}',status='stopped',pid=None,uptime_seconds=None,
        config_files=['config.json'],active_config='config.json',models=[],has_binary=True,
        autostart=False,unreported=0) for index in range(12)]
    page.route(re.compile(r'/api/apps(?:\?|$)'), lambda route: route.fulfill(
        content_type='application/json',body=json.dumps(cards)))
    click_at(page,page.get_by_role('button',name='刷新',exact=True))
    pw_api.expect(page.locator('.app-card')).to_have_count(12)
    page.mouse.move(**screen_point(page.locator('.apps-page'),.6,.5))
    page.mouse.wheel(0,500)
    page.wait_for_function("document.querySelector('.apps-page').scrollTop>100")


def test_flat_editor_palette_and_node_drag(console):
    page,_,_,_=console
    login(page)
    click_at(page,page.locator('.action-btn.edit'))
    pw_api.expect(page.locator('.react-flow')).to_be_visible()
    # Chromium 91 headless does not generate native HTML drag events from CDP
    # mouse input. Exercise the palette's DataTransfer contract explicitly.
    destination=screen_point(page.locator('.flow-container'),.35,.4)
    page.evaluate('''point => {
        const dataTransfer=new DataTransfer();
        document.querySelector('.palette-chip.stream').dispatchEvent(new DragEvent('dragstart',
            {bubbles:true,cancelable:true,dataTransfer}));
        const target=document.elementFromPoint(point.x,point.y);
        for(const type of ['dragover','drop']) target.dispatchEvent(new DragEvent(type,
            {bubbles:true,cancelable:true,dataTransfer,clientX:point.x,clientY:point.y}));
    }''',destination)
    node=page.locator('.react-flow__node')
    pw_api.expect(node).to_have_count(1)
    before=node.get_attribute('style')
    start=screen_point(node,.5,.12)
    drag_at(page,start,dict(x=start['x']+90,y=start['y']+60))
    page.wait_for_timeout(100)
    assert node.get_attribute('style')!=before


@pytest.mark.parametrize('strength',[45,100])
def test_terminal_effects_selection_and_theme_isolation(console,strength):
    page,_,_,messages=console
    login(page)
    click_at(page,page.get_by_role('link',name=re.compile('终端命令行')))
    row=page.locator('.xterm-rows > div').first
    pw_api.expect(row).to_contain_text('EDGE_SELECT')
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    slider=page.get_by_role('slider',name=re.compile('边缘畸变'))
    pw_api.expect(slider).to_have_value('45')
    slider.fill(str(strength))
    page.wait_for_function("""strength=>{
        const surface=document.querySelector('.term-crt-surface');
        const filter=document.querySelector('.term-curvature-defs feDisplacementMap');
        return filter && surface.style.filter && Math.abs(Number(filter.getAttribute('scale'))-
            Math.max(surface.clientWidth,surface.clientHeight)*strength/100*.2)<.001;
    }""",arg=strength)
    page.evaluate('new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)))')
    columns=int(page.locator('.term-pane-size').text_content().split('×')[0])
    drag_at(page,screen_point(row,.1/columns),screen_point(row,11.1/columns))
    page.keyboard.press('Control+Shift+C')
    page.wait_for_function("window.copiedTerminalText==='EDGE_SELECT'")
    page.keyboard.type('echo ready');page.keyboard.press('Enter')
    page.wait_for_timeout(100)
    assert 'echo ready\r' in ''.join(message for message in messages if not message.startswith('\x1b[resize:'))
    click_at(page,page.get_by_role('button',name='白磷屏',exact=True))
    pw_api.expect(page.get_by_role('button',name='白磷屏',exact=True)).to_have_attribute('aria-pressed','true')
    pw_api.expect(page.locator('.term-crt-overlay')).to_be_visible()
    assert page.locator('.xterm-rows').evaluate('e=>getComputedStyle(e).textShadow')!='none'
    assert page.evaluate("getComputedStyle(document.documentElement).getPropertyValue('--accent').trim()")=='#4f8ef7'
    pw_api.expect(page.locator('.sidebar')).to_have_css('text-shadow','none')
    click_at(page,page.get_by_role('button',name=re.compile('新建分屏')))
    pw_api.expect(page.locator('.term-pane')).to_have_count(2)

    pw_api.expect(page.locator('.term-curvature-defs feDisplacementMap')).to_have_count(2)
    page.set_viewport_size({'width':1100,'height':800})
    page.wait_for_function("""()=>[...document.querySelectorAll('.term-pane')].every(pane=>
        Number(pane.querySelector('.term-curvature-defs filter')?.getAttribute('width'))===pane.querySelector('.term-crt-surface').clientWidth)""")
    slider.fill('0')
    pw_api.expect(page.locator('.term-curvature-defs feDisplacementMap')).to_have_count(0)
    slider.fill(str(strength))
    page.get_by_label('CRT 效果',exact=True).uncheck()
    pw_api.expect(page.locator('.term-curvature-defs feDisplacementMap')).to_have_count(0)
    page.get_by_label('CRT 效果',exact=True).check()
    pw_api.expect(page.locator('.term-curvature-defs feDisplacementMap')).to_have_count(2)
    click_at(page,page.get_by_role('link',name=re.compile('程序管理')))
    pw_api.expect(page.locator('.term-crt-overlay')).to_have_count(0)
    pw_api.expect(page.locator('.term-curvature-defs feDisplacementMap')).to_have_count(0)
    assert page.evaluate("getComputedStyle(document.documentElement).getPropertyValue('--accent').trim()")=='#4f8ef7'
    click_at(page,page.get_by_role('link',name=re.compile('终端命令行')))
    pw_api.expect(page.get_by_role('button',name='白磷屏',exact=True)).to_have_attribute('aria-pressed','true')
    pw_api.expect(page.locator('.term-pane')).to_have_count(2)


# Read the rendered reflection's static compositor mask as well as its light buffer.
CANVAS_LIGHT_PIXELS = """async canvas=>{
    let target=canvas;
    const style=getComputedStyle(canvas),mask=style.webkitMaskImage || style.maskImage;
    if(mask && mask!=='none') {
        const bitmap=new Image();bitmap.src=mask.slice(4,-1).replace(/^['"]|['"]$/g,'');
        await bitmap.decode();
        target=document.createElement('canvas');target.width=canvas.width;target.height=canvas.height;
        const ctx=target.getContext('2d');ctx.drawImage(canvas,0,0);
        ctx.globalCompositeOperation='destination-in';ctx.drawImage(bitmap,0,0,target.width,target.height);
    }
    return target.getContext('2d').getImageData(0,0,target.width,target.height).data;
}"""


def canvas_light(page, selector):
    return page.locator(selector).evaluate("""async canvas=>{
        const data=await ("""+CANVAS_LIGHT_PIXELS+""")(canvas);
        const light={alpha:0,red:0,green:0};
        for(let i=0;i<data.length;i+=4){
            light.alpha+=data[i+3]; light.red+=data[i]*data[i+3]; light.green+=data[i+1]*data[i+3];
        }
        return light;
    }""")


def test_terminal_live_reflections_snow_and_phosphor_decay(console):
    page,_,_,_=console
    sockets=[]
    page.route_web_socket('**/ws/terminal?**',lambda ws:sockets.append(ws))
    login(page)
    click_at(page,page.get_by_role('link',name=re.compile('终端命令行')))
    page.wait_for_function("document.querySelector('.term-frame-reflection')?.width>300")
    assert len(sockets)==1
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    for name in ['荧光辉光','光晕扩散']:
        page.get_by_role('slider',name=re.compile(name)).fill('0')
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    columns=int(page.locator('.term-pane-size').text_content().split('×')[0])
    sockets[0].send('\x1b[?25l')
    page.wait_for_timeout(1100)  # Let the initial cursor's own residual light fade.
    # True-color text on the upper edge must be reflected with its actual color.
    sockets[0].send('\x1b[?25l\x1b[2J\x1b[H\x1b[38;2;255;0;0m'+'M'*(columns-1)+'\x1b[0m')
    page.wait_for_timeout(250)
    page.wait_for_function('''()=>{
        const c=document.querySelector('.term-frame-reflection');
        return c.getContext('2d').getImageData(0,0,c.width,c.height).data.some((v,i)=>i%4===3 && v>20);
    }''',timeout=3000)
    red=canvas_light(page,'.term-frame-reflection')
    assert red['alpha']>0 and red['red']>red['green']*5
    assert canvas_light(page,'.term-crt-afterglow')['alpha']==0
    sockets[0].send('\x1b[H\x1b[38;2;0;255;0m'+'W'*(columns-1)+'\x1b[0m')
    page.wait_for_timeout(1100)
    green=canvas_light(page,'.term-frame-reflection')
    assert green['alpha']>0 and green['green']>green['red']*5

    # Read actual pixels from an empty central screen region, away from the cursor.
    bounds=page.locator('.term-crt-surface').bounding_box()
    clip=dict(x=bounds['x']+bounds['width']/2,y=bounds['y']+bounds['height']/2,width=140,height=90)
    snow=page.screenshot(clip=clip)
    page.wait_for_timeout(140)
    assert page.screenshot(clip=clip)!=snow

    # Arm in-browser sampling before clearing, so a busy GPU/driver round trip
    # cannot consume the trail before the first read.
    page.evaluate('''()=>{window.trailSample=(async()=>{
        const c=document.querySelector('.term-crt-afterglow');
        const read=()=>c.getContext('2d').getImageData(0,0,c.width,Math.min(48,c.height))
            .data.reduce((sum,v,i)=>sum+(i%4===3?v:0),0);
        let first=0;
        for(let i=0;i<150 && !first;i++){first=read();if(!first)await new Promise(r=>setTimeout(r,20));}
        const start=performance.now();
        await new Promise(r=>setTimeout(r,180));return [first,read(),performance.now()-start];
    })()}''')
    sockets[0].send('\x1b[2J\x1b[H')
    first,second,elapsed=page.evaluate('window.trailSample')
    assert first>second>=0,(first,second,elapsed)
    page.wait_for_timeout(1000)
    assert canvas_light(page,'.term-crt-afterglow')['alpha']==0
    assert canvas_light(page,'.term-frame-reflection')['alpha']==0

    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    page.get_by_role('slider',name=re.compile('边框反射')).fill('0')
    page.get_by_role('slider',name=re.compile('荧光残影')).fill('0')
    sockets[0].send('\x1b[H'+'M'*(columns-1))
    page.wait_for_timeout(150)
    assert canvas_light(page,'.term-frame-reflection')['alpha']==0
    sockets[0].send('\x1b[2J\x1b[H')
    page.wait_for_timeout(100)
    assert canvas_light(page,'.term-crt-afterglow')['alpha']==0

    page.get_by_role('slider',name=re.compile('边框反射')).fill('55')
    page.get_by_role('slider',name=re.compile('荧光残影')).fill('55')
    page.emulate_media(reduced_motion='reduce')
    sockets[0].send('\x1b[H'+'M'*(columns-1))
    page.wait_for_timeout(150)
    assert canvas_light(page,'.term-frame-reflection')['alpha']>0
    sockets[0].send('\x1b[2J\x1b[H')
    page.wait_for_timeout(100)
    assert canvas_light(page,'.term-crt-afterglow')['alpha']==0
    pw_api.expect(page.locator('.term-crt-noise')).to_have_css('animation-name','none')
    assert len(sockets)==1  # Appearance changes preserve the live terminal session.


def test_cursor_movement_and_css_blink_leave_decaying_light(console):
    page,_,_,_=console
    sockets=[]
    page.route_web_socket('**/ws/terminal?**',lambda ws:sockets.append(ws))
    login(page)
    click_at(page,page.get_by_role('link',name=re.compile('终端命令行')))
    page.wait_for_function("document.querySelector('.term-frame-reflection')?.width>300")
    sockets[0].send('\x1b[2J\x1b[2 q\x1b[?25h\x1b[4;8H')
    pw_api.expect(page.locator('.xterm-rows > div').nth(3).locator('.xterm-cursor')).to_be_visible()
    pw_api.expect(page.locator('.xterm-cursor')).not_to_have_class(re.compile('xterm-cursor-blink'))
    page.wait_for_timeout(1100)
    # Initial font fitting can move the cursor once more after the first render.
    page.wait_for_function('''()=>{
        const c=document.querySelector('.term-crt-afterglow');
        return !c.getContext('2d').getImageData(0,0,c.width,c.height).data.some((v,i)=>i%4===3 && v);
    }''')
    assert canvas_light(page,'.term-crt-afterglow')['alpha']==0
    old=page.locator('.xterm-cursor').evaluate('''e=>{
        const r=e.getBoundingClientRect(),s=e.closest('.term-crt-surface').getBoundingClientRect();
        const c=e.closest('.term-crt-surface').querySelector('canvas');
        return {x:(r.left-s.left)*c.width/s.width,y:(r.top-s.top)*c.height/s.height,
            w:r.width*c.width/s.width,h:r.height*c.height/s.height};
    }''')
    # Arm one-cell sampling before moving; full-canvas GPU readback plus driver
    # round trips may outlast the persistence interval on the device.
    page.locator('.term-crt-afterglow').evaluate('''(c,r)=>{window.cursorMoveSample=(async()=>{
        const read=()=>c.getContext('2d').getImageData(Math.ceil(r.x),Math.ceil(r.y),Math.floor(r.w),Math.floor(r.h))
            .data.reduce((s,v,i)=>s+(i%4===3?v:0),0);
        let first=0;
        for(let i=0;i<150 && !first;i++){first=read();if(!first)await new Promise(r=>setTimeout(r,20));}
        await new Promise(r=>setTimeout(r,120));return [first,read()];
    })()}''',old)
    sockets[0].send('\x1b[4;20H')
    first,second=page.evaluate('window.cursorMoveSample')
    assert first>1000 and first>second>0,(first,second)
    page.wait_for_timeout(1000)
    assert canvas_light(page,'.term-crt-afterglow')['alpha']==0

    # CSS blinking does not emit xterm render events; its off phase must still glow.
    # Read one cursor cell inside the browser, avoiding driver round trips that
    # can span the next blink and create a fresh trail.
    page.evaluate('''()=>{window.blinkSample=(async()=>{
        const c=document.querySelector('.term-crt-afterglow');
        const cursor=document.querySelector('.xterm-cursor').getBoundingClientRect();
        const surface=document.querySelector('.term-phosphor-content').getBoundingClientRect();
        const scale=c.width/surface.width;
        const x=Math.ceil((cursor.left-surface.left)*scale),y=Math.ceil((cursor.top-surface.top)*scale);
        const w=Math.max(1,Math.floor(cursor.width*scale)),h=Math.max(1,Math.floor(cursor.height*scale));
        const read=()=>c.getContext('2d').getImageData(x,y,w,h).data.reduce((s,v,i)=>s+(i%4===3?v:0),0);
        let first=0;
        for(let i=0;i<250 && !first;i++){first=read();if(!first)await new Promise(r=>setTimeout(r,20));}
        await new Promise(r=>setTimeout(r,80));return [first,read()];
    })()}''')
    sockets[0].send('\x1b[1 q')
    first,second=page.evaluate('window.blinkSample')
    assert first>second>0,(first,second)
    sockets[0].send('\x1b[?25l')
    pw_api.expect(page.locator('.xterm-cursor')).to_have_count(0)
    page.wait_for_timeout(1100)
    page.wait_for_function('''()=>{
        const c=document.querySelector('.term-crt-afterglow');
        return !c.getContext('2d').getImageData(0,0,c.width,c.height).data.some((v,i)=>i%4===3 && v);
    }''',timeout=3000)
    assert canvas_light(page,'.term-crt-afterglow')['alpha']==0
    assert canvas_light(page,'.term-frame-reflection')['alpha']==0

    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    page.get_by_label('CRT 效果',exact=True).uncheck()
    assert canvas_light(page,'.term-crt-afterglow')['alpha']==0
    assert len(sockets)==1


def test_curved_liner_joints_interrupt_reflections_at_all_four_corners(console):
    page,_,_,_=console
    sockets=[]
    page.route_web_socket('**/ws/terminal?**',lambda ws:sockets.append(ws))
    login(page)
    click_at(page,page.get_by_role('link',name=re.compile('终端命令行')))
    page.wait_for_function("document.querySelector('.term-frame-reflection')?.width>300")
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    # A wider exposed liner keeps the samples clear of xterm's unlit text padding.
    page.get_by_role('slider',name=re.compile('边缘畸变')).fill('100')
    # A solid lit buffer makes a missing mirror face or overlapping joint unambiguous.
    sockets[0].send('\x1b[?25l\x1b[48;2;240;120;60m\x1b[2J\x1b[H')
    page.wait_for_timeout(1100)
    geometry=page.locator('.term-bezel-joints path').first.evaluate(r"""p=>{
        const ends=[...p.getAttribute('d').matchAll(/M([\d.]+),([\d.]+)L([\d.]+),([\d.]+)/g)]
            .map(m=>m.slice(1).map(Number));
        const s=document.querySelector('.term-crt-surface'),w=s.clientWidth,h=s.clientHeight;
        return ends.map(([ox,oy,x,y])=>{
            const u=2*(x-14)/w-1,v=2*(y-14)/h-1;
            return {sourceX:Math.abs(u*(1+.2*v*v)),sourceY:Math.abs(v*(1+.2*u*u))};
        });
    }""")
    assert len(geometry)==4
    for corner in geometry:
        assert abs(corner['sourceX']-1)<.0001,geometry
        assert abs(corner['sourceY']-1)<.0001,geometry
    # Outside the screen's projected corners the reflected sample is out of
    # bounds. It must stay dark rather than fold back into unrelated text.
    corners=page.locator('.term-frame-reflection').evaluate("""c=>{
        const w=c.width,h=c.height,d=c.getContext('2d').getImageData(0,0,w,h).data;
        return [[false,false],[true,false],[true,true],[false,true]].map(([right,bottom])=>{
            let alpha=0;for(let y=2;y<9;y++)for(let x=2;x<9;x++)
                alpha+=d[((bottom?h-1-y:y)*w+(right?w-1-x:x))*4+3];
            return alpha;
        });
    }""")
    assert corners==[0,0,0,0],corners
    assert canvas_light(page,'.term-frame-reflection')['red']>10000
    pw_api.expect(page.locator('.term-bezel-material polygon')).to_have_count(4)
    page.get_by_role('slider',name=re.compile('边缘畸变')).fill('0')
    pw_api.expect(page.locator('.term-bezel-joints')).to_have_count(0)
    assert canvas_light(page,'.term-frame-reflection')['alpha']==0


def test_reflection_tracks_screen_light_without_new_terminal_output(console):
    page,_,_,_=console
    sockets=[]
    page.route_web_socket('**/ws/terminal?**',lambda ws:sockets.append(ws))
    login(page)
    click_at(page,page.get_by_role('link',name=re.compile('终端命令行')))
    page.wait_for_function("document.querySelector('.term-frame-reflection')?.width>300")
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    for label in ['荧光辉光','扫描线','屏幕暗角','荧光残影','水平同步干扰','文字抖动','RGB 色散','亮度闪烁']:
        page.get_by_role('slider',name=re.compile(label)).fill('0')
    columns=int(page.locator('.term-pane-size').text_content().split('×')[0])
    sockets[0].send('\x1b[?25l\x1b[2J\x1b[H\x1b[38;2;255;255;255m'+'M'*(columns-1))
    page.wait_for_timeout(250)
    baseline=canvas_light(page,'.term-frame-reflection')
    assert baseline['red']>10000
    page.locator('.term-frame-reflection').evaluate('async c=>{window.brightReflection=await ('+CANVAS_LIGHT_PIXELS+')(c)}')
    page.get_by_role('slider',name=re.compile('屏幕暗角')).fill('100')
    page.wait_for_timeout(200)
    dimmed=canvas_light(page,'.term-frame-reflection')
    assert 0<dimmed['red']<baseline['red']*.7
    # Different source positions lose different amounts of light, not a global opacity change.
    spread=page.locator('.term-frame-reflection').evaluate('''async c=>{
        const before=window.brightReflection,after=await ('''+CANVAS_LIGHT_PIXELS+''')(c);
        const ratios=[];
        for(let i=3;i<before.length;i+=4)if(before[i]>100)ratios.push(after[i]/before[i]);
        return Math.max(...ratios)-Math.min(...ratios);
    }''')
    assert spread>.1
    page.get_by_role('slider',name=re.compile('屏幕暗角')).fill('0')
    page.wait_for_timeout(200)
    assert abs(canvas_light(page,'.term-frame-reflection')['red']/baseline['red']-1)<.03
    page.get_by_role('slider',name=re.compile('扫描线')).fill('100')
    page.wait_for_timeout(200)
    assert canvas_light(page,'.term-frame-reflection')['red']<baseline['red']*.95
    page.get_by_role('slider',name=re.compile('扫描线')).fill('0')
    page.get_by_role('slider',name=re.compile('荧光辉光')).fill('100')
    page.wait_for_timeout(200)
    assert canvas_light(page,'.term-frame-reflection')['red']>baseline['red']*1.03

    # Changing vignette while old text is fading must not reset its history buffer.
    page.get_by_role('slider',name=re.compile('荧光残影')).fill('100')
    page.wait_for_timeout(150)
    page.wait_for_function('''()=>{
        const c=document.querySelector('.term-frame-reflection');
        return c.getContext('2d').getImageData(0,0,c.width,c.height).data.some((v,i)=>i%4===3 && v>20);
    }''',timeout=3000)
    sockets[0].send('\x1b[2J\x1b[H')
    page.wait_for_timeout(100)
    page.wait_for_function('''()=>{
        const c=document.querySelector('.term-crt-afterglow');
        return c.getContext('2d').getImageData(0,0,c.width,c.height).data.some((v,i)=>i%4===3 && v>20);
    }''',timeout=3000)
    history=canvas_light(page,'.term-crt-afterglow')['alpha']
    assert history>0
    page.get_by_role('slider',name=re.compile('屏幕暗角')).fill('100')
    page.wait_for_timeout(100)
    assert 0<canvas_light(page,'.term-crt-afterglow')['alpha']<history
    assert len(sockets)==1


def test_crt_bloom_jitter_and_synchronized_flicker(console):
    page,_,_,_=console
    sockets=[]
    page.route_web_socket('**/ws/terminal?**',lambda ws:sockets.append(ws))
    login(page)
    click_at(page,page.get_by_role('link',name=re.compile('终端命令行')))
    page.wait_for_function("document.querySelector('.term-frame-reflection')?.width>300")
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    # Existing saved preferences acquire the new effects without reconnecting.
    for name,value in [('光晕扩散','35'),('亮度闪烁','30'),('文字抖动','35')]:
        pw_api.expect(page.get_by_role('slider',name=re.compile(name))).to_have_value(value)
    for name in ['荧光辉光','光晕扩散','雪花质感','荧光残影','亮度闪烁','文字抖动','水平同步干扰']:
        page.get_by_role('slider',name=re.compile(name)).fill('0')
    columns=int(page.locator('.term-pane-size').text_content().split('×')[0])
    sockets[0].send('\x1b[?25l\x1b[2J\x1b[H'+'M'*(columns-1)+'\r\nPHOSPHOR BLOOM')
    page.wait_for_timeout(700)
    baseline=canvas_light(page,'.term-frame-reflection')['alpha']
    bounds=page.locator('.term-crt-surface').bounding_box()
    clip=dict(x=bounds['x']+10,y=bounds['y']+10,width=300,height=65)
    sharp=page.screenshot(clip=clip)
    page.get_by_role('slider',name=re.compile('光晕扩散')).fill('100')
    page.wait_for_timeout(500)
    assert canvas_light(page,'.term-frame-reflection')['alpha']>baseline
    assert page.screenshot(clip=clip)!=sharp

    page.get_by_role('slider',name=re.compile('亮度闪烁')).fill('100')
    page.get_by_role('slider',name=re.compile('文字抖动')).fill('100')
    # Sample multiple real timeline phases: reflected brightness follows the
    # visible screen, and text shifts without changing rows/columns or layout.
    result=page.evaluate('''()=>{
        const emission=document.querySelector('.term-crt-emission');
        const reflection=document.querySelector('.term-reflection-light');
        const content=document.querySelector('.term-phosphor-content');
        const flickers=[emission,reflection].map(e=>e.getAnimations()[0]);
        const startDiff=Math.abs(flickers[0].startTime-flickers[1].startTime);
        flickers.forEach(a=>a.pause());
        const sizes=[],opacities=[],transforms=[];
        for(const phase of [0,.075,.265,.30,.55,.675,.8,.945]) {
            flickers.forEach(a=>a.currentTime=3700*phase);
            opacities.push([getComputedStyle(emission).opacity,getComputedStyle(reflection).opacity]);
            transforms.push(getComputedStyle(content).transform);
            sizes.push([content.clientWidth,content.clientHeight]);
        }
        flickers.forEach(a=>a.play());
        return {startDiff,sizes,opacities,transforms};
    }''')
    assert result['startDiff']<20
    assert all(a==b for a,b in result['opacities'])
    assert len({a for a,b in result['opacities']})>=3
    assert len({tuple(s) for s in result['sizes']})==1
    assert len(sockets)==1

    # Hidden tabs stop animations, including when history/reflections are disabled.
    page.evaluate("Object.defineProperty(document,'hidden',{configurable:true,get:()=>true});document.dispatchEvent(new Event('visibilitychange'))")
    pw_api.expect(page.locator('.term-crt-emission')).to_have_css('animation-play-state','paused')
    pw_api.expect(page.locator('.term-phosphor-content')).to_have_css('transform','none')
    page.evaluate("delete document.hidden;document.dispatchEvent(new Event('visibilitychange'))")
    page.emulate_media(reduced_motion='reduce')
    for selector in ['.term-crt-emission','.term-reflection-light','.term-phosphor-content','.term-crt-noise']:
        pw_api.expect(page.locator(selector)).to_have_css('animation-name','none')
    page.emulate_media(reduced_motion='no-preference')
    page.get_by_label('CRT 效果',exact=True).uncheck()
    pw_api.expect(page.locator('.term-crt-afterglow')).to_have_css('display','none')
    pw_api.expect(page.locator('.xterm-rows')).to_have_css('text-shadow','none')
    pw_api.expect(page.locator('.term-phosphor-content')).to_have_css('animation-name','none')
    page.get_by_label('CRT 效果',exact=True).check()
    page.reload()
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    for name in ['光晕扩散','亮度闪烁','文字抖动']:
        pw_api.expect(page.get_by_role('slider',name=re.compile(name))).to_have_value('100')


def test_horizontal_sync_rgb_channels_and_local_retro_font(console):
    import base64
    page,_,_,messages=console
    sockets=[]
    page.route_web_socket('**/ws/terminal?**',lambda ws:sockets.append(ws))
    login(page)
    click_at(page,page.get_by_role('link',name=re.compile('终端命令行')))
    page.wait_for_function("window.terminalFontLoaded('VT323',20) && getComputedStyle(document.querySelector('.xterm-rows')).fontFamily.includes('VT323')")
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    pw_api.expect(page.get_by_label('终端字体',exact=True)).to_have_value('font:VT323')
    for name in ['水平同步干扰','RGB 色散']:
        pw_api.expect(page.get_by_role('slider',name=name,exact=True)).to_have_value('20')
    # Loaded font metrics, not a fallback font's cached width, drive PTY sizing.
    retro_columns=int(page.locator('.term-pane-size').text_content().split('×')[0])
    page.get_by_label('终端字体',exact=True).select_option('modern')
    page.wait_for_function("!getComputedStyle(document.querySelector('.xterm-rows')).fontFamily.includes('VT323')")
    page.wait_for_timeout(200)
    modern_columns=int(page.locator('.term-pane-size').text_content().split('×')[0])
    assert retro_columns>modern_columns
    page.get_by_label('终端字体',exact=True).select_option('font:VT323')
    page.wait_for_function("getComputedStyle(document.querySelector('.xterm-rows')).fontFamily.includes('VT323')")
    page.wait_for_timeout(200)
    assert int(page.locator('.term-pane-size').text_content().split('×')[0])==retro_columns
    assert len(sockets)==1

    # Isolate color separation. True white glyphs acquire opposing red/blue
    # fringes; this checks rendered pixels rather than just filter attributes.
    click_at(page,page.get_by_role('button',name='白磷屏',exact=True))
    for name in ['荧光辉光','光晕扩散','亮度闪烁','文字抖动','水平同步干扰','RGB 色散',
                 '扫描线','屏幕暗角','边缘畸变','雪花质感','荧光残影']:
        page.get_by_role('slider',name=re.compile(name)).fill('0')
    sockets[0].send('\x1b[?25l\x1b[2J\x1b[H\x1b[38;2;255;255;255mMMMMMMMMMMMMMMMMMMMM\r\n中文终端 ┌───┐\r\nEDGE_SELECT\x1b[0m')
    pw_api.expect(page.locator('.xterm-rows').first).to_contain_text('中文终端')
    page.wait_for_timeout(400)
    bounds=page.locator('.term-crt-surface').bounding_box()
    clip=dict(x=bounds['x']+10,y=bounds['y']+10,width=220,height=25)
    def fringes():
        data=base64.b64encode(page.screenshot(clip=clip)).decode()
        return page.evaluate('''async data=>{
            const image=new Image();image.src='data:image/png;base64,'+data;await image.decode();
            const c=document.createElement('canvas');c.width=image.width;c.height=image.height;
            const ctx=c.getContext('2d');ctx.drawImage(image,0,0);
            const pixels=ctx.getImageData(0,0,c.width,c.height).data;let count=0;
            for(let i=0;i<pixels.length;i+=4)if(pixels[i]>pixels[i+1]+40 || pixels[i+2]>pixels[i+1]+40)count++;
            return count;
        }''',data)
    baseline=fringes()
    page.get_by_role('slider',name='RGB 色散',exact=True).fill('100')
    assert fringes()>baseline+50
    page.get_by_role('slider',name='边缘畸变',exact=True).fill('100')
    columns=int(page.locator('.term-pane-size').text_content().split('×')[0])
    sockets[0].send('\x1b[H\x1b[38;2;255;255;255m'+'M'*(columns-1))
    page.get_by_role('slider',name='RGB 色散',exact=True).fill('0')
    page.wait_for_timeout(400)
    page.locator('.term-frame-reflection').evaluate('''c=>{
        window.unshiftedReflection=c.getContext('2d').getImageData(0,0,c.width,c.height).data;
    }''')
    page.get_by_role('slider',name='RGB 色散',exact=True).fill('100')
    page.wait_for_timeout(300)
    changed=page.locator('.term-frame-reflection').evaluate('''c=>{
        const pixels=c.getContext('2d').getImageData(0,0,c.width,c.height).data;
        return pixels.reduce((n,v,i)=>n+(Math.abs(v-window.unshiftedReflection[i])>20?1:0),0);
    }''')
    assert changed>100  # Reflections also sample separated color channels.
    page.get_by_role('slider',name='边缘畸变',exact=True).fill('0')

    # The line displacement map changes over time; hidden/reduced-motion states
    # reset its red channel to the exact neutral coordinate (128).
    page.get_by_role('slider',name='水平同步干扰',exact=True).fill('100')
    read_map='''async()=>{
        const e=document.querySelector('.term-signal-defs feImage');
        const image=new Image();image.src=e.getAttribute('href');await image.decode();
        const c=document.createElement('canvas');c.width=1;c.height=128;
        const ctx=c.getContext('2d');ctx.drawImage(image,0,0,1,128);
        return [...ctx.getImageData(0,0,1,128).data].filter((_,i)=>i%4===0);
    }'''
    page.wait_for_function('''async()=>{const values=await ('''+read_map+''')();return values.some(v=>Math.abs(v-128)>20)}''')
    first=page.evaluate(read_map)
    page.wait_for_timeout(200)
    assert page.evaluate(read_map)!=first
    # Capture the coordinates after our mapper, with curvature disabled. Different
    # vertical locations must map to different horizontal source coordinates.
    # Sample enough vertical positions to avoid aliasing a roughly five-cycle
    # wave, and wait for an active burst rather than a random quiet interval.
    page.wait_for_function("""()=>{
        const s=document.querySelector('.term-crt-surface'),r=s.getBoundingClientRect();
        const values=[];const record=e=>values.push(e.clientX-(r.left+100));
        document.addEventListener('mousemove',record);
        for(let i=0;i<32;i++)s.dispatchEvent(new MouseEvent('mousemove',{bubbles:true,clientX:r.left+100,clientY:r.top+r.height*(i+.5)/32}));
        document.removeEventListener('mousemove',record);
        return Math.max(...values)-Math.min(...values)>.5;
    }""")
    page.emulate_media(reduced_motion='reduce')
    # CDP media emulation queues the matchMedia change event asynchronously.
    page.wait_for_function('''async()=>{const values=await ('''+read_map+''')();return values.every(v=>v===128)}''')
    page.emulate_media(reduced_motion='no-preference')
    page.evaluate("Object.defineProperty(document,'hidden',{configurable:true,get:()=>true});document.dispatchEvent(new Event('visibilitychange'))")
    assert all(v==128 for v in page.evaluate(read_map))
    page.evaluate("delete document.hidden;document.dispatchEvent(new Event('visibilitychange'))")
    page.get_by_label('CRT 效果',exact=True).uncheck()
    pw_api.expect(page.locator('.term-signal-defs')).to_have_count(0)
    pw_api.expect(page.locator('.term-phosphor-content')).to_have_css('filter','none')
    assert len(sockets)==1
    page.get_by_label('CRT 效果',exact=True).check()
    page.reload()
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    pw_api.expect(page.get_by_label('终端字体',exact=True)).to_have_value('font:VT323')
    for name in ['水平同步干扰','RGB 色散']:
        pw_api.expect(page.get_by_role('slider',name=name,exact=True)).to_have_value('100')


def test_jitter_and_reflection_commit_the_same_frame(console):
    page,_,_,_=console
    sockets=[];page.route_web_socket('**/ws/terminal?**',lambda ws:sockets.append(ws))
    login(page)
    click_at(page,page.get_by_role('link',name=re.compile('终端命令行')))
    page.wait_for_function("window.terminalFontLoaded('VT323',20) && document.querySelector('.term-frame-reflection')?.width>300")
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    for name in ['荧光残影','水平同步干扰','RGB 色散','亮度闪烁']:
        page.get_by_role('slider',name=re.compile(name)).fill('0')
    page.get_by_role('slider',name=re.compile('文字抖动')).fill('100')
    columns=int(page.locator('.term-pane-size').text_content().split('×')[0])
    sockets[0].send('\x1b[?25l\x1b[2J\x1b[H'+'M'*(columns-1))
    # Wait for xterm to consume the asynchronous socket write and hide its cursor
    # before comparing repeated offsets of an otherwise unchanged light source.
    page.wait_for_function('''count=>{
        const surface=document.querySelector('.term-crt-surface');
        return surface.querySelector('.xterm-rows > div')?.textContent.trim()==='M'.repeat(count)
            && !surface.querySelector('.xterm-cursor');
    }''',arg=columns-1)
    page.wait_for_timeout(500)
    samples=page.evaluate('''async()=>{
        const content=document.querySelector('.term-phosphor-content'),c=document.querySelector('.term-frame-reflection');
        const result=[];
        for(let n=0;n<16;n++){
            const pixels=c.getContext('2d').getImageData(0,0,c.width,32).data;
            let hash=2166136261;for(const value of pixels)hash=Math.imul(hash^value,16777619)>>>0;
            result.push({transform:content.style.transform,screen:content.dataset.signalFrame,reflection:c.dataset.signalFrame,hash});
            await new Promise(r=>setTimeout(r,110));
        }
        return result;
    }''')
    assert all(x['screen']==x['reflection'] for x in samples),samples
    assert len({x['transform'] for x in samples})>=3,samples
    assert len({x['hash'] for x in samples})>=3,samples
    # Repeated screen offsets reproduce the same reflected pixels: no independent
    # animation phase, random displacement, or accumulated positional drift.
    for transform in {x['transform'] for x in samples}:
        assert len({x['hash'] for x in samples if x['transform']==transform})==1,samples
    assert canvas_light(page,'.term-crt-afterglow')['alpha']==0
    assert len(sockets)==1


def test_phosphor_pixel_grid_and_reflected_light(console):
    import base64
    page,_,_,_=console
    sockets=[];page.route_web_socket('**/ws/terminal?**',lambda ws:sockets.append(ws))
    login(page)
    click_at(page,page.get_by_role('link',name=re.compile('终端命令行')))
    page.wait_for_function("window.terminalFontLoaded('VT323',20) && document.querySelector('.term-frame-reflection')?.width>300")
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    grid=page.get_by_role('slider',name=re.compile('像素点阵'))
    size=page.get_by_label('像素尺寸',exact=True)
    pw_api.expect(grid).to_have_value('70');pw_api.expect(size).to_have_value('3')
    for name in ['荧光辉光','光晕扩散','亮度闪烁','文字抖动','水平同步干扰','RGB 色散',
                 '扫描线','屏幕暗角','边缘畸变','雪花质感','荧光残影','像素点阵']:
        page.get_by_role('slider',name=re.compile(name)).fill('0')
    columns=int(page.locator('.term-pane-size').text_content().split('×')[0])
    sockets[0].send('\x1b[?25l\x1b[2J\x1b[H\x1b[48;2;255;255;255m'+((' '*(columns-1)+'\r\n')*6)+'\x1b[0m')
    page.wait_for_timeout(400)
    bounds=page.locator('.term-crt-surface').bounding_box()
    clip=dict(x=bounds['x']+24,y=bounds['y']+24,width=48,height=48)
    def pixels():
        data=base64.b64encode(page.screenshot(clip=clip)).decode()
        return page.evaluate('''async data=>{
            const image=new Image();image.src='data:image/png;base64,'+data;await image.decode();
            const c=document.createElement('canvas');c.width=image.width;c.height=image.height;
            const ctx=c.getContext('2d');ctx.drawImage(image,0,0);
            return [...ctx.getImageData(0,0,c.width,c.height).data].filter((_,i)=>i%4===0);
        }''',data)
    clear=pixels()
    size.select_option('6');grid.fill('100')
    dots=pixels()
    assert sum(dots)<sum(clear)*.8
    assert max(dots)-min(dots)>100
    def periodic_error(values,dx,dy):
        errors=[abs(values[y*48+x]-values[(y+dy)*48+x+dx])
                for y in range(48-dy) for x in range(48-dx)]
        return sum(errors)/len(errors)
    assert periodic_error(dots,6,0)<periodic_error(dots,3,0)*.3
    assert periodic_error(dots,0,6)<periodic_error(dots,0,3)*.3
    size.select_option('3')
    fine=pixels()
    assert periodic_error(fine,3,0)<periodic_error(dots,3,0)*.3

    # With no new terminal output, the reflected light must immediately acquire
    # the same dot attenuation, and return to its old brightness when disabled.
    page.get_by_role('slider',name='边缘畸变',exact=True).fill('100')
    grid.fill('0');page.wait_for_timeout(350)
    before=canvas_light(page,'.term-frame-reflection')['red']
    assert before>10000
    grid.fill('100');page.wait_for_timeout(200)
    after=canvas_light(page,'.term-frame-reflection')['red']
    assert 0<after<before*.85
    grid.fill('0');page.wait_for_timeout(200)
    assert abs(canvas_light(page,'.term-frame-reflection')['red']/before-1)<.03
    grid.fill('70');size.select_option('4')
    page.emulate_media(reduced_motion='reduce')
    pw_api.expect(page.locator('.term-crt-pixels')).to_have_css('opacity','0.7')
    page.get_by_label('CRT 效果',exact=True).uncheck()
    pw_api.expect(page.locator('.term-crt-overlay')).to_have_css('display','none')
    pw_api.expect(size).to_be_disabled()
    page.get_by_label('CRT 效果',exact=True).check()
    assert len(sockets)==1
    page.reload()
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    pw_api.expect(page.get_by_role('slider',name=re.compile('像素点阵'))).to_have_value('70')
    pw_api.expect(page.get_by_label('像素尺寸',exact=True)).to_have_value('4')


@pytest.mark.parametrize('webgl', [True, False])
def test_recessed_screen_brightness_and_all_four_reflections(console,webgl):
    import base64
    page,_,_,_=console
    if not webgl:
        page.evaluate("""()=>{
            const getContext=HTMLCanvasElement.prototype.getContext;
            HTMLCanvasElement.prototype.getContext=function(type,...args){
                return type==='webgl'?null:getContext.call(this,type,...args);
            };
        }""")
    sockets=[];page.route_web_socket('**/ws/terminal?**',lambda ws:sockets.append(ws))
    login(page)
    click_at(page,page.get_by_role('link',name=re.compile('终端命令行')))
    page.wait_for_function("window.terminalFontLoaded('VT323',20) && document.querySelector('.term-frame-reflection')?.width>300")
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    brightness=page.get_by_role('slider',name='字体亮度',exact=True)
    pw_api.expect(brightness).to_have_value('100')
    for label in ['荧光辉光','光晕扩散','亮度闪烁','文字抖动','水平同步干扰','RGB 色散',
                  '扫描线','屏幕暗角','雪花质感','荧光残影','像素点阵']:
        page.get_by_role('slider',name=re.compile(label)).fill('0')
    page.get_by_role('slider',name='边框反射',exact=True).fill('100')
    dimensions=page.locator('.term-crt-surface').evaluate('''s=>({
        x:s.offsetLeft,y:s.offsetTop,w:s.parentElement.clientWidth-s.clientWidth,
        h:s.parentElement.clientHeight-s.clientHeight})''')
    assert dimensions==dict(x=14,y=14,w=28,h=28)
    cols,rows=map(int,page.locator('.term-pane-size').text_content().split('×'))
    # Each edge emits a different color, so missing faces and wrong mirror axes
    # cannot pass by reflecting one bright source onto all four walls.
    output='\x1b[?25l\x1b[2J'
    for row in range(rows):
        output+=f'\x1b[{row+1};1H'
        if row<4: output+='\x1b[38;2;128;0;0m'+'M'*(cols-1)
        elif row>=rows-4: output+='\x1b[38;2;0;0;128m'+'M'*(cols-1)
        else:
            output+='\x1b[38;2;0;128;0m'+'M'*9
            output+=f'\x1b[{row+1};{cols-8}H\x1b[38;2;128;128;0m'+'M'*9
    sockets[0].send(output+'\x1b[0m')
    page.wait_for_function("!document.querySelector('.xterm-cursor') && document.querySelector('.xterm-rows > div')?.textContent.includes('MMMM')")
    page.wait_for_timeout(500)
    sample_js='''(data,w,h,k,inset)=>{
        const sums=Object.fromEntries(['top','bottom','left','right'].map(f=>[f,[0,0,0]]));
        const add=(face,x,y)=>{const i=(y*w+x)*4;for(let c=0;c<3;c++)sums[face][c]+=data[i+c]*data[i+3]/255};
        const iw=w-inset*2,ih=h-inset*2;
        for(let x=Math.ceil(w*.08);x<w*.22;x++){
            const n=2*(x-inset)/iw-1,gap=inset+ih/2*(1-1/(1+k*n*n));
            for(let y=2;y<gap-2;y++){add('top',x,y);add('bottom',x,h-1-y)}
        }
        for(let y=Math.ceil(h*.22);y<h*.27;y++){
            const n=2*(y-inset)/ih-1,gap=inset+iw/2*(1-1/(1+k*n*n));
            for(let x=2;x<gap-2;x++){add('left',x,y);add('right',w-1-x,y)}
        }
        return sums;
    }'''
    for strength in [45,100]:
        page.get_by_role('slider',name='边缘畸变',exact=True).fill(str(strength))
        page.wait_for_timeout(250)
        colors=page.locator('.term-frame-reflection').evaluate('''(c,{sample,strength})=>{
            const data=c.getContext('2d').getImageData(0,0,c.width,c.height).data;
            return eval('('+sample+')')(data,c.width,c.height,strength/500,14*c.width/c.clientWidth);
        }''',dict(sample=sample_js,strength=strength))
        for color in colors.values(): assert max(color)>200,colors
        assert colors['top'][0]>max(colors['top'][1:])*10,colors
        assert colors['bottom'][2]>max(colors['bottom'][:2])*10,colors
        assert colors['left'][1]>max(colors['left'][0],colors['left'][2])*10,colors
        assert min(colors['right'][:2])>colors['right'][2]*10,colors

    # Compare actual composited screenshots, including glass, material and CSS
    # brightness. Subtract the unlit frame to isolate reflection light on each wall.
    def visible_light(value):
        brightness.fill(str(value))
        page.evaluate('new Promise(r=>requestAnimationFrame(()=>requestAnimationFrame(r)))')
        image=base64.b64encode(page.locator('.term-screen').screenshot()).decode()
        return page.evaluate('''async({image,sample})=>{
            const bitmap=new Image();bitmap.src='data:image/png;base64,'+image;await bitmap.decode();
            const c=document.createElement('canvas');c.width=bitmap.width-2;c.height=bitmap.height-2;
            const ctx=c.getContext('2d');ctx.drawImage(bitmap,-1,-1);
            const d=ctx.getImageData(0,0,c.width,c.height).data;
            const sums=eval('('+sample+')')(d,c.width,c.height,.2,14);
            // Include actual screen light in the upper terminal rows as well.
            let screen=0;for(let y=60;y<100;y++)for(let x=200;x<400;x++){
                const i=(y*c.width+x)*4;screen+=d[i]+d[i+1]+d[i+2];
            }
            return {...Object.fromEntries(Object.entries(sums).map(([f,rgb])=>[f,rgb.reduce((a,b)=>a+b,0)])),screen};
        }''',dict(image=image,sample=sample_js))
    dark=visible_light(0);normal=visible_light(100);dim=visible_light(50);bright=visible_light(150)
    for face in normal:
        emitted=normal[face]-dark[face]
        assert emitted>1000,(face,dark,normal)
        assert .30<(dim[face]-dark[face])/emitted<.70,(face,dark,normal,dim)
        assert 1.20<(bright[face]-dark[face])/emitted<1.80,(face,dark,normal,bright)
    assert len(sockets)==1
    page.reload()
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    pw_api.expect(page.get_by_role('slider',name='字体亮度',exact=True)).to_have_value('150')


def test_terminal_custom_pixel_font_size(console):
    page,_,_,messages=console
    sockets=[]
    def terminal(ws):
        sockets.append(ws);ws.on_message(lambda message:messages.append(message))
    page.route_web_socket('**/ws/terminal?**',terminal)
    login(page)
    click_at(page,page.get_by_role('link',name=re.compile('终端命令行')))
    page.wait_for_function("window.terminalFontLoaded('VT323',20) && document.querySelector('.term-frame-reflection')?.width>300")
    size=page.get_by_role('spinbutton',name='字号',exact=True)
    pw_api.expect(size).to_have_value('20')
    original_columns=int(page.locator('.term-pane-size').text_content().split('×')[0])
    size.fill('17.5')
    page.wait_for_function("getComputedStyle(document.querySelector('.xterm-rows')).fontSize==='17.5px'")
    page.wait_for_function("document.querySelector('.term-pane-size').textContent.split('×')[0].trim()>"+str(original_columns))
    size.fill('')
    assert page.locator('.xterm-rows').evaluate('e=>getComputedStyle(e).fontSize')=='17.5px'
    size.press('Tab');pw_api.expect(size).to_have_value('17.5')
    size.fill('999');size.press('Enter');pw_api.expect(size).to_have_value('128')
    page.wait_for_function("getComputedStyle(document.querySelector('.xterm-rows')).fontSize==='128px'")
    size.fill('27');size.press('Enter')
    page.wait_for_function("getComputedStyle(document.querySelector('.xterm-rows')).fontSize==='27px'")
    click_at(page,page.get_by_role('button',name=re.compile('新建分屏')))
    page.wait_for_function("document.querySelectorAll('.xterm-rows').length===2 && [...document.querySelectorAll('.xterm-rows')].every(r=>getComputedStyle(r).fontSize==='27px')")
    assert len(sockets)==2
    page.reload()
    pw_api.expect(page.get_by_role('spinbutton',name='字号',exact=True)).to_have_value('27')
    page.wait_for_function("getComputedStyle(document.querySelector('.xterm-rows')).fontSize==='27px'")


@pytest.mark.skipif(os.environ.get('CHROMIUM_HEADED') != '1', reason='Native fullscreen needs headed Chromium; set CHROMIUM_HEADED=1 with DISPLAY/XAUTHORITY.')
def test_terminal_native_fullscreen_preserves_sessions(console):
    page,_,_,messages=console
    sockets=[]
    def terminal(ws):
        sockets.append(ws);ws.on_message(lambda message:messages.append(message))
    page.route_web_socket('**/ws/terminal?**',terminal)
    login(page)
    click_at(page,page.get_by_role('link',name=re.compile('终端命令行')))
    page.wait_for_function("window.terminalFontLoaded('VT323',20) && document.querySelector('.term-frame-reflection')?.width>300")
    size=page.get_by_role('spinbutton',name='字号',exact=True)
    size.fill('27');size.press('Enter')
    page.wait_for_function("getComputedStyle(document.querySelector('.xterm-rows')).fontSize==='27px'")
    sockets[0].send('\x1b[2J\x1b[HFULLSCREEN_SESSION_KEEP')
    pw_api.expect(page.locator('.xterm-rows')).to_contain_text('FULLSCREEN_SESSION_KEEP')
    click_at(page,page.get_by_role('button',name=re.compile('新建分屏')))
    pw_api.expect(page.locator('.term-pane')).to_have_count(2)
    page.wait_for_function("[...document.querySelectorAll('.xterm-rows')].every(r=>getComputedStyle(r).fontSize==='27px')")
    before=page.locator('.term-pane-body').first.bounding_box()
    click_at(page,page.get_by_role('button',name='全屏',exact=True))
    page.wait_for_function("document.fullscreenElement===document.querySelector('.terminal-grid')")
    page.wait_for_function('''()=>{
        const r=document.querySelector('.terminal-grid').getBoundingClientRect();
        return r.x===0 && r.y===0 && Math.abs(r.width-innerWidth)<2 && Math.abs(r.height-innerHeight)<2;
    }''')
    pw_api.expect(page.get_by_role('button',name='退出全屏 · Esc',exact=True)).to_be_visible()
    page.wait_for_function("[...document.querySelectorAll('.term-pane-bar')].every(e=>getComputedStyle(e).display==='none')")
    # Both sessions and their text/CRT geometry survive the actual Fullscreen API.
    assert len(sockets)==2
    page.wait_for_function("[...document.querySelectorAll('.term-pane')].every(p=>Number(p.querySelector('.term-curvature-defs filter').getAttribute('width'))===p.querySelector('.term-crt-surface').clientWidth)")
    pw_api.expect(page.locator('.xterm-rows').first).to_contain_text('FULLSCREEN_SESSION_KEEP')
    assert page.locator('.term-curvature-defs feDisplacementMap').count()==2
    click_at(page,page.get_by_role('button',name='退出全屏 · Esc',exact=True))
    page.wait_for_function('!document.fullscreenElement')
    page.wait_for_function("[...document.querySelectorAll('.term-pane-bar')].every(e=>getComputedStyle(e).display!=='none')")
    assert len(sockets)==2
    page.wait_for_function("document.activeElement?.classList.contains('xterm-helper-textarea')")
    click_at(page,page.get_by_role('button',name='全屏',exact=True))
    page.wait_for_function("document.fullscreenElement===document.querySelector('.terminal-grid')")
    page.keyboard.press('Escape')
    page.wait_for_function('!document.fullscreenElement')
    page.wait_for_function("document.activeElement?.classList.contains('xterm-helper-textarea')")
    page.keyboard.type('echo still-connected');page.keyboard.press('Enter')
    commands=''.join(m for m in messages if not m.startswith('\x1b[resize:'))
    assert commands=='echo still-connected\r',commands


@pytest.mark.parametrize('legacy', [False, True])
def test_departure_font_default_migration_and_live_metrics(console,legacy):
    page,_,_,_=console
    prefs=dict(profile='green',enabled=True)
    if legacy: prefs.update(font='vt323',glow=63,curvature=45)
    page.evaluate("prefs=>localStorage.setItem('terminal-appearance-v1',JSON.stringify(prefs))",prefs)
    sockets=[];page.route_web_socket('**/ws/terminal?**',lambda ws:sockets.append(ws))
    login(page)
    click_at(page,page.get_by_role('link',name=re.compile('终端命令行')))
    picker=page.get_by_label('终端字体',exact=True)
    pw_api.expect(picker).to_have_value('font:Departure Mono')
    size=page.get_by_role('spinbutton',name='字号',exact=True);size.fill('22');size.press('Enter')
    page.wait_for_function("""()=>{
        const rows=document.querySelector('.xterm-rows');
        return window.terminalFontLoaded('Departure Mono',22) && getComputedStyle(rows).fontFamily.includes('Departure Mono')
            && getComputedStyle(rows).fontSize==='22px';
    }""")
    page.wait_for_function("""()=>{
        const row=document.querySelector('.xterm-rows > div');
        const cols=Number(document.querySelector('.term-pane-size').textContent.split('×')[0]);
        return Math.abs(row.getBoundingClientRect().width/cols-14)<.05;
    }""")
    columns=int(page.locator('.term-pane-size').text_content().split('×')[0])
    sockets[0].send('\x1b[2J\x1b[H'+'M'*(columns-1)+'\r\n中文终端 ┌───┐\r\n(base) ywei_linux@Laowei2001:~$ ls')
    pw_api.expect(page.locator('.xterm-rows')).to_contain_text('中文终端')
    page.wait_for_function("""()=>{
        const c=document.querySelector('.term-frame-reflection');
        return c.getContext('2d').getImageData(0,0,c.width,c.height).data.some((v,i)=>i%4===3&&v>20);
    }""")
    if legacy:
        assert page.evaluate("JSON.parse(localStorage.getItem('terminal-appearance-v1')).glow")==63
    picker.select_option('font:VT323')
    page.wait_for_function("window.terminalFontLoaded('VT323',22) && getComputedStyle(document.querySelector('.xterm-rows')).fontFamily.includes('VT323')")
    page.reload()
    pw_api.expect(page.get_by_label('终端字体',exact=True)).to_have_value('font:VT323')
    page.get_by_label('终端字体',exact=True).select_option('font:Departure Mono')
    page.wait_for_function("getComputedStyle(document.querySelector('.xterm-rows')).fontFamily.includes('Departure Mono')")
    pw_api.expect(page.get_by_role('spinbutton',name='字号',exact=True)).to_have_value('22')


def test_font_folder_names_drive_picker_and_loading(console):
    page,_,_,_=console
    root=Path(__file__).resolve().parents[1]/'src/assets/fonts'
    extensions={'.ttf','.otf','.woff','.woff2'}
    expected={folder.name for folder in root.iterdir() if folder.is_dir()
              and any(file.is_file() and file.suffix.lower() in extensions for file in folder.rglob('*'))}
    login(page)
    click_at(page,page.get_by_role('link',name=re.compile('终端命令行')))
    picker=page.get_by_label('终端字体',exact=True)
    pw_api.expect(picker).to_be_visible()
    assert set(picker.locator('option').all_text_contents())==expected|{'现代等宽'}
    folder=os.environ.get('TERMINAL_TEST_FONT_FOLDER','Departure Mono')
    assert folder in expected
    picker.select_option('font:'+folder)
    size=page.get_by_role('spinbutton',name='字号',exact=True);size.fill('22');size.press('Enter')
    page.wait_for_function("""folder=>{
        const rows=document.querySelector('.xterm-rows'),style=getComputedStyle(rows);
        const first=style.fontFamily.split(',')[0].trim(),name=first.startsWith('\"')?JSON.parse(first):first;
        return [...document.fonts].some(face=>face.family===name && face.status==='loaded')
            && document.fonts.check('22px '+first) && style.fontFamily.includes(folder) && style.fontSize==='22px';
    }""",arg=folder)
    pw_api.expect(page.get_by_role('link',name=folder+' 字体许可',exact=True)).to_be_visible()
    page.reload()
    pw_api.expect(page.get_by_label('终端字体',exact=True)).to_have_value('font:'+folder)
