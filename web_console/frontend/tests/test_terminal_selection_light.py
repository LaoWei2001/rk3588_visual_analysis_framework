"""Selection backgrounds emit and retain light without any terminal output."""
import re
import pytest
from test_terminal_display import console, login, click_at, drag_at, screen_point, canvas_light


@pytest.mark.parametrize('material', ['rough', 'smooth'])
def test_selection_background_reflects_and_leaves_a_decaying_trail(console, material):
    page,_,_,_=console
    sockets=[]
    page.route_web_socket('**/ws/terminal?**', lambda ws:sockets.append(ws))
    login(page)
    click_at(page,page.get_by_role('link',name=re.compile('终端命令行')))
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    for label in ['文字抖动','水平同步干扰','RGB 色散','亮度闪烁','屏幕暗角','扫描线','像素点阵']:
        page.get_by_role('slider',name=re.compile(label)).fill('0')
    page.get_by_label('边框材质',exact=True).select_option(material)
    page.get_by_role('slider',name='边框反射',exact=True).fill('100')
    page.get_by_role('slider',name='荧光残影',exact=True).fill('100')
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    # A blank buffer isolates selection light from glyphs, ANSI backgrounds and cursor.
    sockets[0].send('\x1b[?25l\x1b[2J\x1b[H')
    page.wait_for_function('''()=>!document.querySelector('.xterm-cursor') &&
        ['.term-crt-afterglow','.term-frame-reflection'].every(selector=>{
            const c=document.querySelector(selector);return c.width>0&&!c.getContext('2d').getImageData(0,0,c.width,c.height).data.some((v,i)=>i%4===3&&v);
        })''',timeout=6000)
    assert canvas_light(page,'.term-frame-reflection')['alpha']==0
    rows=page.locator('.xterm-rows > div')
    drag_at(page,screen_point(rows.nth(0),.02),screen_point(rows.nth(3),.85))
    page.wait_for_function("document.querySelector('.xterm-selection > div')?.style.width !== '0px' && document.querySelectorAll('.xterm-selection > div').length>0")
    page.wait_for_function('''()=>{const c=document.querySelector('.term-frame-reflection');
        return c.getContext('2d').getImageData(0,0,c.width,c.height).data.some((v,i)=>i%4===3&&v>0)}''')
    selected=canvas_light(page,'.term-frame-reflection')['alpha']
    assert selected>100
    # Dragging itself moves the selection; let those transient trails settle.
    page.wait_for_function('''()=>{const c=document.querySelector('.term-crt-afterglow');
        return !c.getContext('2d').getImageData(0,0,c.width,c.height).data.some((v,i)=>i%4===3&&v)}''',timeout=6000)
    selected=canvas_light(page,'.term-frame-reflection')['alpha']
    page.locator('.term-screen').screenshot(path=f'/tmp/terminal-selection-{material}.png')
    # Focus changes use xterm's inactive selection color without new websocket data.
    page.get_by_role('button',name=re.compile('显示调校')).focus()
    page.wait_for_timeout(350)
    inactive=canvas_light(page,'.term-frame-reflection')['alpha']
    assert 0<inactive<selected
    page.locator('.xterm-helper-textarea').focus()
    page.wait_for_timeout(150)
    # Observe the interior of a selected blank row before cancelling. The old
    # background must persist at that location and keep its actual display color.
    page.evaluate(r'''()=>{
        const c=document.querySelector('.term-crt-afterglow'),content=c.parentElement.getBoundingClientRect();
        const row=document.querySelectorAll('.xterm-rows > div')[1].getBoundingClientRect();
        const x=Math.floor((row.left-content.left+row.width*.3)*c.width/content.width);
        const y=Math.floor((row.top-content.top+row.height*.5)*c.height/content.height);
        window.selectionColor=getComputedStyle(document.querySelector('.xterm-selection > div')).backgroundColor.match(/[\d.]+/g).slice(0,3).map(Number);
        window.selectionTrail=(async()=>{
            const read=()=>Array.from(c.getContext('2d').getImageData(x,y,1,1).data);
            let first;
            for(let i=0;i<150;i++){first=read();if(first[3])break;await new Promise(r=>setTimeout(r,10));}
            await new Promise(r=>setTimeout(r,180));return {first,second:read(),color:window.selectionColor};
        })();
    }''')
    click_at(page,rows.nth(7))
    trail=page.evaluate('window.selectionTrail')
    assert trail['first'][3]>trail['second'][3]>0,trail
    assert max(abs(a-b) for a,b in zip(trail['first'][:3],trail['color']))<5,trail
    page.wait_for_timeout(1800)
    assert canvas_light(page,'.term-crt-afterglow')['alpha']==0
    assert canvas_light(page,'.term-frame-reflection')['alpha']==0
    assert len(sockets)==1
