"""Optical falloff and phosphor structure, measured from browser-rendered pixels."""
import base64
import re

from test_terminal_display import console, login, click_at, pw_api


def open_terminal(page):
    sockets = []
    page.route_web_socket('**/ws/terminal?**', lambda ws: sockets.append(ws))
    login(page)
    click_at(page, page.get_by_role('link', name=re.compile('终端命令行')))
    page.wait_for_function("document.querySelector('.term-frame-reflection')?.width>300")
    click_at(page, page.get_by_role('button', name=re.compile('显示调校')))
    return sockets


READ_VIGNETTE = '''async e=>{
    const url=e.style.getPropertyValue('--crt-vignette-texture');
    const image=new Image();image.src=url.slice(5,-2);await image.decode();
    const canvas=document.createElement('canvas');canvas.width=image.width;canvas.height=image.height;
    const ctx=canvas.getContext('2d');ctx.drawImage(image,0,0);
    const d=ctx.getImageData(0,0,canvas.width,canvas.height).data;
    const at=(x,y)=>d[(Math.min(canvas.height-1,Math.floor(y*canvas.height))*canvas.width+Math.min(canvas.width-1,Math.floor(x*canvas.width)))*4+3];
    return {url,w:canvas.width,h:canvas.height,aspect:e.clientWidth/e.clientHeight,
        center:at(.5,.5),inner:at(.65,.5),edge:at(.5,.02),corner:at(.02,.02),
        line:Array.from({length:51},(_,i)=>at(.5+i*.01,.5))};
}'''


def test_vignette_preserves_center_and_follows_glass_geometry(console):
    page, _, _, _ = console
    open_terminal(page)
    surface = page.locator('.term-crt-surface')
    vignette = page.get_by_role('slider', name='屏幕暗角', exact=True)
    initial = surface.evaluate(READ_VIGNETTE)
    assert initial['center'] <= 2 and initial['inner'] < 5, initial
    assert initial['corner'] > initial['edge'] > initial['inner']
    vignette.fill('100')
    page.wait_for_function("old=>document.querySelector('.term-crt-surface').style.getPropertyValue('--crt-vignette-texture')!==old", arg=initial['url'])
    strong = surface.evaluate(READ_VIGNETTE)
    assert strong['center'] <= 2 and strong['inner'] < 12
    assert strong['corner'] > strong['edge'] + 15
    assert strong['edge'] > initial['edge'] + 15
    assert all(b >= a-2 for a,b in zip(strong['line'], strong['line'][1:])), strong['line']
    pw_api.expect(page.get_by_role('slider', name='边缘畸变', exact=True)).to_have_count(0)
    page.set_viewport_size({'width': 1100, 'height': 1000})
    page.wait_for_function("old=>document.querySelector('.term-crt-surface').style.getPropertyValue('--crt-vignette-texture')!==old", arg=strong['url'])
    resized = surface.evaluate(READ_VIGNETTE)
    assert abs(resized['w']/resized['h']-resized['aspect']) < .02
    assert max(resized['w'], resized['h']) <= 384
    vignette.fill('0')
    page.wait_for_function("!document.querySelector('.term-crt-surface').style.getPropertyValue('--crt-vignette-texture')")


def test_optics_maps_are_static_while_crt_keeps_animating(console):
    page, _, _, _ = console
    open_terminal(page)
    page.wait_for_timeout(500)
    result = page.evaluate('''async()=>{
        const prototype=CanvasRenderingContext2D.prototype, old=prototype.putImageData;
        let uploads=0;prototype.putImageData=function(...args){uploads++;return old.apply(this,args)};
        const surface=document.querySelector('.term-crt-surface');
        const before=surface.style.getPropertyValue('--crt-vignette-texture');
        await new Promise(resolve=>setTimeout(resolve,1200));prototype.putImageData=old;
        return {uploads,unchanged:before===surface.style.getPropertyValue('--crt-vignette-texture'),
            running:document.querySelector('.term-screen').getAnimations({subtree:true}).some(a=>a.playState==='running')};
    }''')
    assert result == {'uploads': 0, 'unchanged': True, 'running': True}


def test_phosphor_changes_structure_without_dimming_midtone_light(console):
    page, _, _, _ = console
    sockets = open_terminal(page)
    for name in ['荧光辉光','光晕扩散','亮度闪烁','文字抖动','水平同步干扰','RGB 色散',
                 '扫描线','屏幕暗角','雪花质感','荧光残影','像素点阵']:
        page.get_by_role('slider',name=re.compile(name)).fill('0')
    columns = int(page.locator('.term-pane-size').text_content().split('×')[0])
    sockets[0].send('\x1b[?25l\x1b[2J\x1b[H\x1b[48;2;128;128;128m' + (' '*(columns-1)+'\r\n')*8 + '\x1b[0m')
    page.wait_for_timeout(250)
    rect = page.locator('.term-crt-surface').bounding_box()
    clip = dict(x=rect['x']+rect['width']/2-48,y=rect['y']+40,width=96,height=96)
    def sample():
        image = base64.b64encode(page.screenshot(clip=clip)).decode()
        return page.evaluate('''async data=>{
            const image=new Image();image.src='data:image/png;base64,'+data;await image.decode();
            const c=document.createElement('canvas');c.width=image.width;c.height=image.height;
            const ctx=c.getContext('2d');ctx.drawImage(image,0,0);
            const values=[...ctx.getImageData(0,0,c.width,c.height).data].filter((_,i)=>i%4===0);
            const mean=values.reduce((a,b)=>a+b,0)/values.length;
            return {mean,range:Math.max(...values)-Math.min(...values),
                deviation:Math.sqrt(values.reduce((a,b)=>a+(b-mean)**2,0)/values.length)};
        }''', image)
    plain = sample()
    page.get_by_label('像素尺寸', exact=True).select_option('6')
    page.get_by_role('slider', name='像素点阵', exact=True).fill('100')
    grains = sample()
    assert .94 < grains['mean']/plain['mean'] < 1.06, (plain, grains)
    assert grains['range'] > 90 and grains['deviation'] > plain['deviation']+20, (plain, grains)
    page.get_by_role('slider', name='像素点阵', exact=True).fill('0')
    restored = sample()
    assert abs(restored['mean']-plain['mean']) < 2
    # The fixed curved view can include edge antialiasing. Disabling grains
    # must restore the same structure as the original view, not an ideal plane.
    assert abs(restored['deviation']-plain['deviation']) < 1, (plain, grains, restored)


def test_crt_optics_visual_preview(console):
    page, _, _, _ = console
    sockets = open_terminal(page)
    page.get_by_label('终端字体', exact=True).select_option('font:IBM VGA 8x16')
    page.get_by_role('spinbutton', name='字号', exact=True).fill('32')
    page.wait_for_function("getComputedStyle(document.querySelector('.xterm-rows')).fontFamily.includes('IBM VGA 8x16')")
    page.emulate_media(reduced_motion='reduce')
    sockets[0].send('\x1b[?25l\x1b[2J\x1b[H'
                    'IBM VGA 8x16  /  CRT DISPLAY\r\n'
                    'ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789\r\n'
                    '\x1b[32mPhosphor grains / soft glass corners\x1b[0m\r\n'
                    '\x1b[37;46m READABLE COLOR BACKGROUND \x1b[0m\r\n'
                    '\x1b[7m INVERSE TEXT \x1b[0m\r\n'
                    'root@rk3588:~# _')
    pw_api.expect(page.locator('.xterm-rows')).to_contain_text('root@rk3588')
    click_at(page, page.get_by_role('button', name=re.compile('显示调校')))
    page.locator('.term-screen').screenshot(path='/tmp/terminal-optics-default.png')
    click_at(page, page.get_by_role('button', name=re.compile('显示调校')))
    page.get_by_role('slider', name='屏幕暗角', exact=True).fill('85')
    page.get_by_label('像素尺寸', exact=True).select_option('4')
    click_at(page, page.get_by_role('button', name=re.compile('显示调校')))
    page.locator('.term-screen').screenshot(path='/tmp/terminal-optics-strong.png')
    assert len(sockets) == 1
