"""Rendered colors and same-frame phosphor handoff, not just buffer attributes."""
import re
from test_terminal_display import console, login, click_at, pw_api


def test_displayed_colors_and_clear_handoff_match_the_terminal(console):
    page,_,_,_=console
    sockets=[]
    page.route_web_socket('**/ws/terminal?**',lambda ws:sockets.append(ws))
    login(page)
    click_at(page,page.get_by_role('link',name=re.compile('终端命令行')))
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    for label in ['文字抖动','水平同步干扰','RGB 色散','亮度闪烁','屏幕暗角','扫描线','像素点阵','荧光辉光','光晕扩散','雪花质感']:
        page.get_by_role('slider',name=re.compile(label)).fill('0')
    page.get_by_role('slider',name='荧光残影',exact=True).fill('100')
    page.get_by_label('边框材质',exact=True).select_option('smooth')
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    page.evaluate('''()=>{
        window.sampledGlyphs={};const original=CanvasRenderingContext2D.prototype.fillText;
        CanvasRenderingContext2D.prototype.fillText=function(text,x,...rest){
            if(text.startsWith('HELP_')&&x>=0)window.sampledGlyphs[text]={color:this.fillStyle,alpha:this.globalAlpha};
            return original.call(this,text,x,...rest);
        };
    }''')
    sockets[0].send('\x1b[?25l\x1b[2J\x1b[H'
                    '\x1b[7;36mHELP_INVERSE\x1b[0m\r\n'
                    '\x1b[97;44mHELP_WHITE\x1b[0m\r\n'
                    '\x1b[2;37mHELP_DIM\x1b[0m\r\n'
                    '\x1b[38;2;240;240;240;48;2;50;50;50mHELP_RGB\x1b[0m')
    page.wait_for_function("Object.keys(window.sampledGlyphs).length===4")
    colors=page.evaluate('''()=>{
        const c=document.createElement('canvas').getContext('2d');
        return Array.from(document.querySelectorAll('.xterm-rows span')).filter(e=>e.textContent.startsWith('HELP_')).map(e=>{
            c.fillStyle=getComputedStyle(e).color;
            return {text:e.textContent,display:c.fillStyle,sample:window.sampledGlyphs[e.textContent]};
        });
    }''')
    assert len(colors)==4,colors
    for color in colors:
        assert color['sample']['color']==color['display'],color
        assert color['sample']['alpha']==1,color  # Dim alpha already lives in the resolved color.
    page.wait_for_timeout(1800)
    page.locator('.term-screen').screenshot(path='/tmp/terminal-emission-colors.png')
    # MutationObserver runs after xterm's render callbacks, before the next paint.
    # The first visible cleared frame must already contain persistence, with no
    # intervening dark frame and no later increase in retained light.
    page.evaluate('''()=>{
        const rows=document.querySelector('.xterm-rows'),c=document.querySelector('.term-crt-afterglow');
        const reflection=document.querySelector('.term-frame-reflection');
        const reflected=()=>reflection.getContext('2d').getImageData(0,0,reflection.width,reflection.height).data.reduce((s,v,i)=>s+(i%4===3?v:0),0);
        const read=()=>c.getContext('2d').getImageData(0,0,c.width,c.height).data.reduce((s,v,i)=>s+(i%4===3?v:0),0);
        window.clearSamples=new Promise(resolve=>{
            const observer=new MutationObserver(()=>{
                if(rows.textContent.trim())return;
                observer.disconnect();const samples=[read()];window.reflectionSamples=[reflected()];
                const timer=setInterval(()=>{
                    // An unchanged signal still invalidates reflection caches,
                    // exercising transitions that used to re-brighten the echo.
                    if(samples.length%2===0)document.querySelector('.term-crt-surface').dispatchEvent(new Event('terminal-signal-change'));
                    samples.push(read());window.reflectionSamples.push(reflected());
                    if(samples.length===12){clearInterval(timer);resolve(samples)}
                },50);
            });
            observer.observe(rows,{childList:true,subtree:true,characterData:true});
        });
    }''')
    sockets[0].send('\x1b[0m\x1b[2J\x1b[H')
    samples=page.evaluate('window.clearSamples')
    assert samples[0]>1000,samples
    assert all(b<=a for a,b in zip(samples,samples[1:])),samples
    assert 0<samples[-1]<samples[0]*.5,samples
    reflected=page.evaluate('window.reflectionSamples')
    assert reflected[0]>0 and reflected[-1]<reflected[0],reflected
    assert all(b<=a for a,b in zip(reflected,reflected[1:])),reflected
    page.wait_for_timeout(1800)
    page.wait_for_function('''()=>{const c=document.querySelector('.term-crt-afterglow');
        return !c.getContext('2d').getImageData(0,0,c.width,c.height).data.some((v,i)=>i%4===3&&v)}''')
    assert len(sockets)==1


def test_glass_edge_feathers_only_the_boundary_and_leaves_room_for_shadow(console):
    page,_,_,_=console
    login(page)
    click_at(page,page.get_by_role('link',name=re.compile('终端命令行')))
    page.wait_for_function("document.querySelector('feImage[result=glassCoverage]')?.getAttribute('href')")
    mask=page.locator('feImage[result=glassCoverage]').evaluate('''async e=>{
        const image=new Image();image.src=e.getAttribute('href');await image.decode();
        const c=document.createElement('canvas');c.width=image.width;c.height=image.height;
        const ctx=c.getContext('2d');ctx.drawImage(image,0,0);const d=ctx.getImageData(0,0,c.width,c.height).data;
        const at=(x,y)=>d[(y*c.width+x)*4+3],y=Math.floor(c.height/2);
        return {edge:[0,1,2,3,4].map(x=>at(x,y)),center:at(Math.floor(c.width/2),y)};
    }''')
    assert 0<mask['edge'][0]<mask['edge'][1]<255,mask
    assert mask['edge'][3:]==[255,255] and mask['center']==255,mask
    geometry=page.locator('.term-crt-surface').evaluate('''s=>{
        const f=s.parentElement.querySelector('.term-curvature-defs filter');
        const warp=f.querySelector('feDisplacementMap');
        return {x:Number(f.getAttribute('x')),margin:Number(f.getAttribute('width'))-s.clientWidth,
            clipX:Number(warp.getAttribute('x')),clipWidth:Number(warp.getAttribute('width'))-s.clientWidth};
    }''')
    assert geometry['x']<0 and geometry['margin']==-2*geometry['x'],geometry
    assert geometry['clipX']==0 and geometry['clipWidth']==0,geometry
    page.locator('.term-screen').screenshot(path='/tmp/terminal-glass-boundary.png')
