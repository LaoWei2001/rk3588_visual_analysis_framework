"""A persisted recess setting must drive layout, optics and interaction together."""
from pathlib import Path
import re
import subprocess

from test_terminal_display import console, login, click_at, drag_at, screen_point, pw_api


def test_recess_bounds_and_small_pane_geometry(tmp_path):
    frontend = Path(__file__).resolve().parents[1]
    subprocess.run([str(frontend/'node_modules/.bin/tsc'), 'src/features/terminal/terminalReflectionTransport.ts',
                    '--target', 'es2020', '--module', 'commonjs', '--skipLibCheck',
                    '--outDir', str(tmp_path)], cwd=frontend, check=True)
    script = r'''
const assert=require('assert/strict'),path=require('path');
const g=require(path.join(process.argv[1],'terminalBezelGeometry.js'));
const m=require(path.join(process.argv[1],'terminalReflectionTransport.js'));
assert.equal(g.SCREEN_RECESS,42);
for(const value of [undefined,null,'72',NaN,Infinity,-Infinity])assert.equal(g.resolveScreenRecess(value),42);
assert.equal(g.resolveScreenRecess(-10),12);assert.equal(g.resolveScreenRecess(900),72);
for(const [w,h] of [[1280,850],[640,480],[180,100],[60,60]])for(const requested of [12,28,42,72]){
  const inset=g.fitScreenRecess(requested,w,h);
  assert.ok(inset>0&&inset<=requested&&inset<=Math.min(w,h)*.22);
  const scene=m.reflectionScene(w,h,15,inset),iw=w-inset*2,ih=h-inset*2;
  assert.equal(scene.depth,inset*2.5);assert.ok(iw>0&&ih>0);
  const center=scene.trace([0,0,0],[0,0,-1]);assert.ok(center&&center.distance>0);
  const corners=g.bezelJoints(w,h,.03,inset);
  for(const [, [x,y]] of corners){
    const u=2*(x-inset)/iw-1,v=2*(y-inset)/ih-1;
    assert.ok(Math.abs(Math.abs(u*(1+.03*v*v))-1)<1e-8);
    assert.ok(Math.abs(Math.abs(v*(1+.03*u*u))-1)<1e-8);
  }
}
const shallow=m.buildReflectionTransport(480,360,15,'smooth',12);
const deep=m.buildReflectionTransport(480,360,15,'smooth',72);
assert.notStrictEqual(shallow,deep);
assert.strictEqual(m.buildReflectionTransport(480,360,15,'smooth',72),deep);
assert.ok(deep.positions.length>shallow.positions.length);
for(const map of [shallow,deep])for(let i=0;i<map.paths.length;i+=3){
  assert.ok(Number.isFinite(map.paths[i+2])&&map.paths[i+2]>=0);
  if(map.paths[i+2])assert.ok(map.paths[i]>=0&&map.paths[i]<=1&&map.paths[i+1]>=0&&map.paths[i+1]<=1);
}
'''
    subprocess.run(['node', '-e', script, str(tmp_path)], check=True)


def test_recess_adjustment_persists_and_keeps_selection_and_sessions(console):
    page,_,_,messages = console
    sockets = []
    def terminal(ws):
        sockets.append(ws)
        ws.on_message(lambda message: messages.append(message))
        ws.send('\x1b[?25l\x1b[2J\x1b[HEDGE_SELECT retained terminal session\r\n\x1b[31mDEPTH REFLECTION\x1b[0m')
    page.route_web_socket('**/ws/terminal?**', terminal)
    login(page)
    click_at(page, page.get_by_role('link', name=re.compile('终端命令行')))
    page.wait_for_function("window.terminalFontLoaded('VT323',20)")
    click_at(page, page.get_by_role('button', name=re.compile('显示调校')))
    depth = page.get_by_role('slider', name=re.compile('屏幕嵌入深度'))
    pw_api.expect(depth).to_have_value('42')
    page.emulate_media(reduced_motion='reduce')

    def set_depth(value):
        depth.fill(str(value))
        page.wait_for_function('''value=>{
            const screen=document.querySelector('.term-screen'),s=screen.querySelector('.term-crt-surface');
            const svg=screen.querySelector('.term-bezel-material');
            return Number(screen.dataset.screenRecess)===value&&s.offsetLeft===value&&s.offsetTop===value
                &&screen.clientWidth-s.clientWidth===2*value&&svg.viewBox.baseVal.width===screen.clientWidth;
        }''', arg=value)
        page.wait_for_function('''()=>{const c=document.querySelector('.term-frame-reflection');
            return c.width>0&&c.getContext('2d').getImageData(0,0,c.width,c.height).data.some((v,i)=>i%4===3&&v>0)}''')

    set_depth(72)
    page.wait_for_function("JSON.parse(localStorage.getItem('terminal-appearance-v1')).recess===72")
    shadow = page.locator('feDropShadow[in=curvedScreen]')
    assert float(shadow.get_attribute('stdDeviation')) == 8
    columns = int(page.locator('.term-pane-size').text_content().split('×')[0])
    click_at(page, page.get_by_role('button', name=re.compile('显示调校')))
    row = page.locator('.xterm-rows > div').first
    drag_at(page, screen_point(row,.1/columns), screen_point(row,11.1/columns))
    page.keyboard.press('Control+Shift+C')
    page.wait_for_function("window.copiedTerminalText==='EDGE_SELECT'")
    page.locator('.term-screen').screenshot(path='/tmp/terminal-recess-72.png')
    click_at(page, page.get_by_role('button', name=re.compile('显示调校')))
    set_depth(12)
    assert int(page.locator('.term-pane-size').text_content().split('×')[0]) > columns
    assert float(shadow.get_attribute('stdDeviation')) == 1.5
    assert len(sockets) == 1
    set_depth(72)
    click_at(page, page.get_by_role('button', name=re.compile('新建分屏')))
    page.wait_for_function("document.querySelectorAll('.term-screen').length===2&&[...document.querySelectorAll('.term-screen')].every(s=>Number(s.dataset.screenRecess)===72)")
    assert len(sockets) == 2
    page.reload()
    click_at(page, page.get_by_role('button', name=re.compile('显示调校')))
    pw_api.expect(page.get_by_role('slider', name=re.compile('屏幕嵌入深度'))).to_have_value('72')
    # Reset also cancels an in-flight range update.
    page.get_by_role('slider', name=re.compile('屏幕嵌入深度')).fill('18')
    click_at(page, page.get_by_role('button', name='恢复默认', exact=True))
    page.wait_for_function("JSON.parse(localStorage.getItem('terminal-appearance-v1')).recess===42")
    page.wait_for_timeout(250)
    pw_api.expect(page.get_by_role('slider', name=re.compile('屏幕嵌入深度'))).to_have_value('42')
    page.get_by_label('CRT 效果', exact=True).uncheck()
    pw_api.expect(page.get_by_role('slider', name=re.compile('屏幕嵌入深度'))).to_be_disabled()
    page.get_by_label('CRT 效果', exact=True).check()
    page.get_by_role('slider', name=re.compile('屏幕嵌入深度')).fill('72')
    page.wait_for_function("JSON.parse(localStorage.getItem('terminal-appearance-v1')).recess===72")
    page.set_viewport_size(dict(width=620,height=480))
    page.wait_for_function('''()=>[...document.querySelectorAll('.term-screen')].every(s=>{
        const inset=Number(s.dataset.screenRecess),inner=s.querySelector('.term-crt-surface');
        return inset<=Math.min(s.clientWidth,s.clientHeight)*.22+.01&&inner.clientWidth>0&&inner.clientHeight>0;
    })''')
