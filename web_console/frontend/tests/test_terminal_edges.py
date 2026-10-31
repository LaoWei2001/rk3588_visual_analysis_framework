"""Regression for opaque viewport edges passing through the CRT signal filter."""
import base64
import re
from test_terminal_display import console, login, click_at

def test_rgb_split_does_not_outline_empty_terminal_background(console):
    page,_,_,_=console
    login(page)
    click_at(page,page.get_by_role('link',name=re.compile('终端命令行')))
    page.wait_for_function("document.querySelector('.term-frame-reflection')?.width>300")
    click_at(page,page.get_by_role('button',name=re.compile('显示调校')))
    page.get_by_role('slider',name='RGB 色散',exact=True).fill('100')
    page.emulate_media(reduced_motion='reduce')
    page.wait_for_timeout(250)
    # This band contains no text. Showing an empty terminal must not change
    # the glass background or introduce dark RGB fringes at the viewport edges.
    rect=page.locator('.term-screen').bounding_box()
    clip=dict(x=rect['x'],y=rect['y']+rect['height']*.5,width=rect['width'],height=24)
    visible=base64.b64encode(page.screenshot(clip=clip)).decode()
    page.locator('.term-phosphor-content').evaluate("e=>e.style.visibility='hidden'")
    hidden=base64.b64encode(page.screenshot(clip=clip)).decode()
    diff=page.evaluate('''async images=>{
        let width=0;
        const pixels=await Promise.all(images.map(async data=>{
            const image=new Image();image.src='data:image/png;base64,'+data;await image.decode();
            const c=document.createElement('canvas');c.width=image.width;c.height=image.height;width=c.width;
            const ctx=c.getContext('2d');ctx.drawImage(image,0,0);
            return ctx.getImageData(0,0,c.width,c.height).data;
        }));
        let peak=0,total=0;
        for(let i=0;i<pixels[0].length;i++){
            const delta=Math.abs(pixels[0][i]-pixels[1][i]),x=Math.floor(i/4)%width;
            // Inspect the actual viewport edges; interior noise can change its
            // subpixel rasterization when a transparent filter layer is hidden.
            if(x<64||x>=width-64)peak=Math.max(peak,delta);
            total+=delta;
        }
        return {peak,mean:total/pixels[0].length};
    }''',[visible,hidden])
    # Allow a few 8-bit levels from compositing transparent filter layers.
    # The opaque viewport produces a 16-level edge and >1.5 mean difference.
    assert diff['peak']<=3 and diff['mean']<.3,diff
