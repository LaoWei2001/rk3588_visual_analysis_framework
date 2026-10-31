"""Both renderers evaluate the same bounded radiance transport."""
from pathlib import Path
import re
import subprocess
import pytest
from test_terminal_display import console


def load_renderer_modules(page, tmp_path):
    frontend=Path(__file__).resolve().parents[1]
    subprocess.run([str(frontend/'node_modules/.bin/tsc'),'src/features/terminal/terminalReflectionRaster.ts',
                    '--target','es2020','--module','es2020','--skipLibCheck','--outDir',str(tmp_path)],cwd=frontend,check=True)
    def module(route):
        name=route.request.url.rsplit('/',1)[-1]
        if not name.endswith('.js'): name+='.js'
        route.fulfill(content_type='text/javascript',body=(tmp_path/name).read_text())
    page.route(re.compile(r'/physics/[^/]+$'),module)


@pytest.mark.parametrize('material', ['rough','smooth'])
def test_gpu_and_software_transport_agree_without_gpu_readback(console, tmp_path, material):
    page,_,_,_=console
    load_renderer_modules(page,tmp_path)
    result=page.evaluate('''async material=>{
        const {createReflectionRaster}=await import('/physics/terminalReflectionRaster.js');
        const {buildReflectionTransport}=await import('/physics/terminalReflectionTransport.js');
        const transport=buildReflectionTransport(720,540,45,material);
        const source=document.createElement('canvas');source.width=360;source.height=270;
        const ctx=source.getContext('2d');
        ctx.fillStyle='#804020';ctx.fillRect(0,0,360,270);
        ctx.fillStyle='#a0d0e0';ctx.fillRect(40,40,120,80);
        const oldDraw=WebGLRenderingContext.prototype.drawArrays;
        const oldRead=CanvasRenderingContext2D.prototype.getImageData;
        let draws=0,reads=0;
        WebGLRenderingContext.prototype.drawArrays=function(...a){draws++;return oldDraw.apply(this,a)};
        CanvasRenderingContext2D.prototype.getImageData=function(...a){reads++;return oldRead.apply(this,a)};
        const gpu=createReflectionRaster();gpu.configure(transport);
        const start=performance.now(),image=gpu.draw(source),gpuTime=performance.now()-start;
        const gpuReads=reads;
        const snapshot=c=>{const out=document.createElement('canvas');out.width=c.width;out.height=c.height;
            const ink=out.getContext('2d');ink.drawImage(c,0,0);return ink.getImageData(0,0,c.width,c.height).data};
        const a=snapshot(image);
        const oldContext=HTMLCanvasElement.prototype.getContext;
        HTMLCanvasElement.prototype.getContext=function(kind,...args){return kind==='webgl'?null:oldContext.call(this,kind,...args)};
        const cpu=createReflectionRaster();cpu.configure(transport);
        HTMLCanvasElement.prototype.getContext=oldContext;
        const begin=performance.now(),other=cpu.draw(source),cpuTime=performance.now()-begin;
        const b=snapshot(other);
        const warmStart=performance.now();for(let i=0;i<3;i++)cpu.draw(source);
        const cpuWarm=(performance.now()-warmStart)/3;
        let peak=0,sum=0;
        for(let i=0;i<a.length;i+=4)for(let c=0;c<4;c++){
            const delta=Math.abs(c===3?a[i+3]-b[i+3]:a[i+c]*a[i+3]/255-b[i+c]*b[i+3]/255);
            sum+=delta;peak=Math.max(peak,delta);
        }
        const linear=v=>v<=.04045?v/12.92:((v+.055)/1.055)**2.4;
        const energy=data=>{let total=0;for(let i=0;i<data.length;i+=4)total+=linear(data[i]/255*data[i+3]/255);return total};
        ctx.fillStyle='rgb(128,128,128)';ctx.fillRect(0,0,360,270);
        const normalEnergy=energy(snapshot(gpu.draw(source)));
        ctx.fillStyle='rgb(64,64,64)';ctx.fillRect(0,0,360,270);
        const dimEnergy=energy(snapshot(gpu.draw(source)));
        const energyRatio=dimEnergy/normalEnergy,expectedRatio=linear(64/255)/linear(128/255);
        ctx.clearRect(0,0,360,270);
        const dark=snapshot(gpu.draw(source));
        gpu.dispose();cpu.dispose();
        WebGLRenderingContext.prototype.drawArrays=oldDraw;
        CanvasRenderingContext2D.prototype.getImageData=oldRead;
        return {draws,gpuReads,gpuTime,cpuTime,cpuWarm,energyRatio,expectedRatio,peak,mean:sum/a.length,dark:dark.some(v=>v!==0),
            maxAlpha:a.reduce((m,v,i)=>i%4===3?Math.max(m,v):m,0)};
    }''',material)
    print(result)
    assert result['draws']==(8 if material=='rough' else 4) and result['gpuReads']==0,result
    assert result['mean']<.5 and result['peak']<6,result
    assert 0<result['maxAlpha']<128 and not result['dark'],result

    assert abs(result['energyRatio']-result['expectedRatio'])<.02,result


def test_reconstruction_removes_sampling_checkerboard_without_crossing_faces(console, tmp_path):
    page,_,_,_=console
    load_renderer_modules(page,tmp_path)
    result=page.evaluate('''async()=>{
        const {createReflectionRaster}=await import('/physics/terminalReflectionRaster.js');
        const {TRANSPORT_SAMPLES,LOBE_SAMPLES}=await import('/physics/terminalReflectionTransport.js');
        const width=64,height=32,positions=Uint32Array.from({length:width*height},(_,i)=>i);
        const faces=Uint8Array.from(positions,i=>i%width<32?1:2);
        const paths=new Float32Array(positions.length*TRANSPORT_SAMPLES*3);
        const source=document.createElement('canvas');source.width=2;source.height=1;
        const ink=source.getContext('2d');
        const snapshot=c=>{const out=document.createElement('canvas');out.width=width;out.height=height;
            const ctx=out.getContext('2d');ctx.drawImage(c,0,0);return ctx.getImageData(0,0,width,height).data};
        const sample=(data,x,y,c)=>data[(y*width+x)*4+c]/255*data[(y*width+x)*4+3]/255;
        const linear=v=>v<=.04045?v/12.92:((v+.055)/1.055)**2.4;
        const results=[];
        for(const useGPU of [true,false]){
            const oldContext=HTMLCanvasElement.prototype.getContext;
            if(!useGPU)HTMLCanvasElement.prototype.getContext=function(kind,...args){return kind==='webgl'?null:oldContext.call(this,kind,...args)};
            const renderer=createReflectionRaster();HTMLCanvasElement.prototype.getContext=oldContext;
            for(const p of positions){const at=p*TRANSPORT_SAMPLES*3;
                paths[at]=((p%width+Math.floor(p/width))%2)?.25:.75;
                paths[at+1]=.5;paths[at+2]=1/LOBE_SAMPLES;}
            renderer.configure({width,height,positions,faces,paths});
            ink.fillStyle='#fff';ink.fillRect(0,0,1,1);ink.fillStyle='#000';ink.fillRect(1,0,1,1);
            const check=snapshot(renderer.draw(source)),values=[];
            for(let y=5;y<27;y++)for(let x=5;x<27;x++)values.push(sample(check,x,y,0));
            const range=(Math.max(...values)-Math.min(...values))*255;
            const energy=values.reduce((a,v)=>a+linear(v),0)/values.length;
            // Each wall has a different light source. Its edge must retain its
            // own color even where the reconstruction kernel straddles the seam.
            for(const p of positions)paths[p*TRANSPORT_SAMPLES*3]=p%width<32?.25:.75;
            renderer.configure({width,height,positions,faces,paths});
            ink.fillStyle='#f00';ink.fillRect(1,0,1,1);
            const walls=snapshot(renderer.draw(source));
            results.push({useGPU,range,energy,expected:.5/LOBE_SAMPLES,
                white:sample(walls,31,16,1),red:sample(walls,32,16,0),leak:sample(walls,32,16,1)});
            renderer.dispose();
        }
        return results;
    }''')
    print(result)
    for item in result:
        assert item['range']<2,item
        assert abs(item['energy']-item['expected'])<.002,item
        assert item['white']>.1 and item['red']>.1 and item['leak']<.005,item


def test_final_material_composite_adds_linear_light_on_gpu_and_cpu(console, tmp_path):
    page,_,_,_=console
    load_renderer_modules(page,tmp_path)
    results=page.evaluate('''async()=>{
        const {createReflectionRaster}=await import('/physics/terminalReflectionRaster.js');
        const width=16,height=8,positions=Uint32Array.from({length:width*height},(_,i)=>i);
        const faces=new Uint8Array(positions.length).fill(1),base=new Uint8Array(positions.length*4);
        const paths=new Float32Array(positions.length*3),material=[90,96,80],emitted=[128,64,192];
        for(const p of positions){base.set([...material,255],p*4);paths.set([.5,.5,16/255],p*3)}
        const source=document.createElement('canvas');source.width=width;source.height=height;
        const ink=source.getContext('2d');ink.fillStyle='rgb('+emitted.join(',')+')';ink.fillRect(0,0,width,height);
        const linear=v=>v<=.04045?v/12.92:((v+.055)/1.055)**2.4;
        const srgb=v=>v<=.0031308?v*12.92:1.055*v**(1/2.4)-.055;
        const snapshot=c=>{const out=document.createElement('canvas');out.width=width;out.height=height;
            const ctx=out.getContext('2d');ctx.drawImage(c,0,0);return ctx.getImageData(0,0,width,height).data};
        const results=[];
        for(const useGPU of [true,false]){
            const oldContext=HTMLCanvasElement.prototype.getContext;
            if(!useGPU)HTMLCanvasElement.prototype.getContext=function(kind,...args){return kind==='webgl'?null:oldContext.call(this,kind,...args)};
            const renderer=createReflectionRaster();HTMLCanvasElement.prototype.getContext=oldContext;
            renderer.configure({width,height,positions,faces,paths,base,samples:1,weightScale:1,radius:0,sigma:1});
            for(const strength of [0,.25,1]){
                // Passing draw's own canvas also exercises aliasing in the software fallback.
                const raw=renderer.draw(source),overlay=snapshot(renderer.composite(raw,strength));
                let peak=0,maxAlpha=0;
                for(const p of positions){const at=p*4,a=overlay[at+3]/255;maxAlpha=Math.max(maxAlpha,overlay[at+3]);
                    for(let c=0;c<3;c++){
                        const actual=overlay[at+c]*a+material[c]*(1-a);
                        const expected=srgb(linear(material[c]/255)+linear(emitted[c]/255)*(16/255)*strength)*255;
                        peak=Math.max(peak,Math.abs(actual-expected));
                    }
                }
                results.push({useGPU,strength,peak,maxAlpha});
            }
            renderer.dispose();
        }
        return results;
    }''')
    for result in results:
        assert result['peak']<2,result
        if result['strength']==0:
            assert result['maxAlpha']==0,result
        else:
            assert 0<result['maxAlpha']<30,result


def test_smooth_pixel_coverage_preserves_sharp_reflection_edges(console, tmp_path):
    page,_,_,_=console
    load_renderer_modules(page,tmp_path)
    results=page.evaluate('''async()=>{
        const {createReflectionRaster}=await import('/physics/terminalReflectionRaster.js');
        const {TERMINAL_REFLECTION_MATERIALS}=await import('/physics/terminalReflectionMaterial.js');
        const {MIRROR_PIXEL_SAMPLES}=await import('/physics/terminalReflectionTransport.js');
        const settings=TERMINAL_REFLECTION_MATERIALS.smooth,width=64,height=32;
        const positions=Uint32Array.from({length:width*height},(_,i)=>i),faces=new Uint8Array(positions.length).fill(1);
        const paths=new Float32Array(positions.length*settings.samples*3);
        for(const p of positions)for(let j=0;j<settings.samples;j++){
            const [dx,dy]=MIRROR_PIXEL_SAMPLES[j];
            paths.set([(p%width+.5+dx)/width,(Math.floor(p/width)+.5+dy)/height,.04/settings.samples],
                (p*settings.samples+j)*3);
        }
        const source=document.createElement('canvas');source.width=128;source.height=64;
        const ctx=source.getContext('2d');ctx.fillStyle='#fff';ctx.fillRect(0,0,64,64);
        ctx.fillStyle='#000';ctx.fillRect(64,0,64,64);
        const results=[];
        for(const useGPU of [true,false]){
            const original=HTMLCanvasElement.prototype.getContext;
            if(!useGPU)HTMLCanvasElement.prototype.getContext=function(kind,...args){return kind==='webgl'?null:original.call(this,kind,...args)};
            const renderer=createReflectionRaster();HTMLCanvasElement.prototype.getContext=original;
            renderer.configure({width,height,positions,faces,paths,...settings});
            const image=renderer.draw(source),out=document.createElement('canvas');out.width=width;out.height=height;
            const ink=out.getContext('2d');ink.drawImage(image,0,0);
            const data=ink.getImageData(0,0,width,height).data;
            const linear=v=>v<=.04045?v/12.92:((v+.055)/1.055)**2.4;
            const energy=x=>{const i=(16*width+x)*4;return linear(data[i]/255*data[i+3]/255)};
            results.push({useGPU,white:energy(16),lastWhite:energy(31),firstBlack:energy(32),dark:energy(48)});
            renderer.dispose();
        }
        return results;
    }''')
    for result in results:
        assert abs(result['white']-.04)<.002,result
        assert abs(result['lastWhite']-result['white'])<.001,result
        assert result['firstBlack']<.0005 and result['dark']==0,result
