"""Auto-darkening must not recolor DOM/SVG independently of Canvas reflections."""
import base64
import re
import pytest
from test_terminal_display import console, login, click_at, pw_api


@pytest.mark.parametrize('material',['rough','smooth'])
def test_auto_dark_preserves_terminal_and_bezel_pixels(console, material):
    page,_,_,_=console
    cdp=page.context.new_cdp_session(page)
    try:
        cdp.send('Emulation.setAutoDarkModeOverride',{'enabled':False})
    except pw_api.Error:
        pytest.skip('Requires a browser with the Auto Dark Theme emulation API')
    sockets=[]
    page.route_web_socket('**/ws/terminal?**',lambda ws:sockets.append(ws))
    login(page)
    click_at(page,page.get_by_role('link',name=re.compile('终端命令行')))
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    for label in ['文字抖动','水平同步干扰','RGB 色散','亮度闪烁','屏幕暗角','扫描线','像素点阵','荧光辉光','光晕扩散','雪花质感','荧光残影']:
        page.get_by_role('slider',name=re.compile(label)).fill('0')
    page.get_by_label('边框材质',exact=True).select_option(material)
    page.get_by_role('slider',name='边框反射',exact=True).fill('100')
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    page.emulate_media(reduced_motion='reduce')
    page.wait_for_timeout(300)
    rows=int(page.locator('.term-pane-size').text_content().split('×')[1])
    sockets[0].send('\x1b[?25l\x1b[2J\x1b[H\x1b[30;46mHELP_BLACK\x1b[0m'
                    f'\x1b[{rows};1H\x1b[38;2;255;255;255;48;2;0;0;88mHELP_WHITE\x1b[0m')
    pw_api.expect(page.locator('.xterm-rows')).to_contain_text('HELP_WHITE')
    page.wait_for_timeout(300)
    screen=page.locator('.term-screen')
    def snapshot(stage):
        return base64.b64encode(screen.screenshot(path=f'/tmp/terminal-browser-colors-{material}-{stage}.png')).decode()
    before=snapshot('normal')
    cdp.send('Emulation.setAutoDarkModeOverride',{'enabled':True})
    protected=snapshot('protected')
    # Demonstrate the original mismatch in this very browser. Automatic dark
    # recoloring changes painted glyphs and the SVG liner, but not computed color.
    glyph=page.locator('.xterm-rows span').filter(has_text='HELP_BLACK')
    original_color=glyph.evaluate('e=>getComputedStyle(e).color')
    page.add_style_tag(content='.term-screen { color-scheme: normal !important; }')
    unprotected=snapshot('unprotected')
    assert glyph.evaluate('e=>getComputedStyle(e).color')==original_color
    result=page.evaluate('''async images=>{
        const data=await Promise.all(images.map(async src=>{
            const image=new Image();image.src='data:image/png;base64,'+src;await image.decode();
            const c=document.createElement('canvas');c.width=image.width;c.height=image.height;
            const ctx=c.getContext('2d');ctx.drawImage(image,0,0);return ctx.getImageData(0,0,c.width,c.height).data;
        }));
        const delta=b=>data[0].reduce((sum,v,i)=>sum+Math.abs(v-b[i]),0)/data[0].length;
        return {protected:delta(data[1]),unprotected:delta(data[2])};
    }''',[before,protected,unprotected])
    assert result['protected']<.1,result
    assert result['unprotected']>2,result
    assert len(sockets)==1
