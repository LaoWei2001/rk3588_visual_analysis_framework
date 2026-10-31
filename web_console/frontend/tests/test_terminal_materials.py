"""Selectable, persistent liner materials with shared shell sessions."""
import re
from test_terminal_display import console, login, click_at, pw_api, canvas_light


def test_material_switch_persists_and_keeps_terminal_sessions(console):
    page,_,_,_=console
    sockets=[]
    page.route_web_socket('**/ws/terminal?**',lambda ws:sockets.append(ws))
    login(page)
    click_at(page,page.get_by_role('link',name=re.compile('终端命令行')))
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    material=page.get_by_label('边框材质',exact=True)
    pw_api.expect(material).to_have_value('rough')
    page.get_by_label('终端字体',exact=True).select_option('font:IBM VGA 8x16')
    page.get_by_role('spinbutton',name='字号',exact=True).fill('32')
    page.wait_for_function("window.terminalFontLoaded('IBM VGA 8x16',32)")
    for name in ['荧光辉光','光晕扩散','亮度闪烁','文字抖动','水平同步干扰','RGB 色散','扫描线','屏幕暗角','雪花质感','荧光残影','像素点阵']:
        page.get_by_role('slider',name=re.compile(name)).fill('0')
    page.get_by_role('slider',name='边框反射',exact=True).fill('100')
    page.emulate_media(reduced_motion='reduce')
    sockets[0].send('\x1b[?25l\x1b[2J\x1b[HWHITE 0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ\r\n'
                    '\x1b[31mRED   0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ\r\n'
                    '\x1b[32mGREEN 0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ\r\n'
                    '\x1b[34mBLUE  0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ\r\n'
                    '\x1b[0mMATERIAL_SWITCH_SESSION')
    pw_api.expect(page.locator('.xterm-rows')).to_contain_text('MATERIAL_SWITCH_SESSION')
    def ready():
        page.wait_for_function('''()=>{const c=document.querySelector('.term-frame-reflection');
            return c&&c.width>0&&c.getContext('2d').getImageData(0,0,c.width,c.height).data.some((v,i)=>i%4===3&&v>0)}''')
    ready()
    page.wait_for_function("Math.max(document.querySelector('.term-frame-reflection').width,document.querySelector('.term-frame-reflection').height)===384")
    assert page.locator('.term-frame-reflection').evaluate("e=>getComputedStyle(e).filter")=='none'
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    page.wait_for_timeout(250)
    page.locator('.term-screen').screenshot(path='/tmp/terminal-material-rough.png')
    rough=canvas_light(page,'.term-frame-reflection')
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    material.select_option('smooth')
    pw_api.expect(page.locator('.term-screen')).to_have_attribute('data-bezel-material','smooth')
    ready()
    page.wait_for_function("Math.max(document.querySelector('.term-frame-reflection').width,document.querySelector('.term-frame-reflection').height)===768")
    assert page.locator('.term-frame-reflection').evaluate("e=>getComputedStyle(e).filter")=='none'
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    page.wait_for_timeout(250)
    page.locator('.term-screen').screenshot(path='/tmp/terminal-material-smooth.png')
    smooth=canvas_light(page,'.term-frame-reflection')
    assert smooth['alpha']>0 and smooth!=rough,(rough,smooth)
    assert len(sockets)==1
    pw_api.expect(page.locator('.xterm-rows')).to_contain_text('MATERIAL_SWITCH_SESSION')
    assert page.evaluate("JSON.parse(localStorage.getItem('terminal-appearance-v1')).reflectionMaterial")=='smooth'
    click_at(page,page.get_by_role('button',name=re.compile('新建分屏')))
    pw_api.expect(page.locator('.term-screen')).to_have_count(2)
    for screen in page.locator('.term-screen').all():
        pw_api.expect(screen).to_have_attribute('data-bezel-material','smooth')
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    page.get_by_label('CRT 效果',exact=True).uncheck()
    pw_api.expect(material).to_be_disabled()
    page.get_by_label('CRT 效果',exact=True).check()
    click_at(page,page.get_by_role('link',name=re.compile('程序管理')))
    click_at(page,page.get_by_role('link',name=re.compile('终端命令行')))
    assert len(sockets)==2
    for screen in page.locator('.term-screen').all():
        pw_api.expect(screen).to_have_attribute('data-bezel-material','smooth')
    page.reload()
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    pw_api.expect(page.get_by_label('边框材质',exact=True)).to_have_value('smooth')
    click_at(page,page.get_by_role('button',name='恢复默认',exact=True))
    pw_api.expect(page.get_by_label('边框材质',exact=True)).to_have_value('rough')
    # Old preferences without a material, and invalid stored values, use rough.
    page.evaluate("(()=>{const p=JSON.parse(localStorage.getItem('terminal-appearance-v1'));p.reflectionMaterial='unknown';localStorage.setItem('terminal-appearance-v1',JSON.stringify(p))})()")
    page.reload()
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    pw_api.expect(page.get_by_label('边框材质',exact=True)).to_have_value('rough')
