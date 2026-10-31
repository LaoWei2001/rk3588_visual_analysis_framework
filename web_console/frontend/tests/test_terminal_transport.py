"""Physics invariants of the cached liner transport, independent of browser styling."""
from pathlib import Path
import subprocess


def test_reflection_laws_visibility_and_energy(tmp_path):
    frontend = Path(__file__).resolve().parents[1]
    subprocess.run([str(frontend/'node_modules/.bin/tsc'), 'src/features/terminal/terminalReflectionTransport.ts',
                    '--target', 'es2020', '--module', 'commonjs', '--skipLibCheck',
                    '--outDir', str(tmp_path)], cwd=frontend, check=True)
    script = r'''
const assert=require('assert/strict');
const m=require(process.argv[1]);
const near=(a,b,eps=1e-6)=>assert.ok(Math.abs(a-b)<eps,`${a} != ${b}`);
const dot=(a,b)=>a.reduce((s,x,i)=>s+x*b[i],0);
const n=[0,0,1],v=[.6,0,.8],r=m.reflectedDirection(v,n);
near(dot(n,v),dot(n,r));near(r[0],-.6);near(Math.hypot(...r),1);
near(m.dielectricFresnel(1),.04);near(m.dielectricFresnel(0),1);
assert.ok(m.dielectricFresnel(.1)>m.dielectricFresnel(.8));
const scene=m.reflectionScene(1280,850,45,28);
assert.equal(scene.surface(640,425),undefined);
const left=scene.surface(14,425),right=scene.surface(1266,425);
const ray=m.reflectedDirection(left.view,left.normal);
near(dot(ray,left.normal),dot(left.view,left.normal));
const a=scene.trace(left.point,ray);
const b=scene.trace(right.point,m.reflectedDirection(right.view,right.normal));
assert.ok(a && b);near(a.u,1-b.u,1e-4);near(a.v,b.v,1e-4);
assert.equal(scene.trace(left.point,[0,0,1]),undefined);
assert.equal(scene.trace(left.point,[-1,0,-.01]),undefined);
// This is a perspective ray intersection, not a copy at equal edge distance.
assert.ok(Math.abs(a.u-(28-14)/(1280-56))>.0005);
const map=m.buildReflectionTransport(1280,850,45,'rough',28);
assert.ok(map.positions.length>1000 && Math.max(map.width,map.height)<=384);
let misses=0,lit=0;
for(let i=0;i<map.positions.length;i++){
  let energy=0;
  for(let j=0;j<m.TRANSPORT_SAMPLES;j++){
    const p=(i*m.TRANSPORT_SAMPLES+j)*3,u=map.paths[p],v=map.paths[p+1],weight=map.paths[p+2];
    assert.ok(Number.isFinite(weight)&&weight>=0);
    if(weight){assert.ok(u>=0&&u<=1&&v>=0&&v<=1);energy+=weight}else misses++;
  }
  // White incident radiance cannot gain energy; missed rays remain losses.
  assert.ok(energy<=1);if(energy>0)lit++;
}
assert.ok(misses>0&&lit>map.positions.length*.8);
const smooth=m.buildReflectionTransport(1280,850,45,'smooth',28);
assert.notStrictEqual(smooth,map);
assert.strictEqual(m.buildReflectionTransport(1280,850,45,'smooth',28),smooth);
assert.strictEqual(m.buildReflectionTransport(1280,850,45,'rough',28),map);
assert.notStrictEqual(m.buildReflectionTransport(1280,850,45,'smooth',42),smooth);
assert.equal(smooth.samples,4);assert.equal(smooth.weightScale,4);
assert.equal(smooth.paths.length,smooth.positions.length*smooth.samples*3);
assert.ok(smooth.width>map.width && smooth.radius<map.radius);
let hits=0,blocked=0;
for(let i=0;i<smooth.positions.length;i+=31){
  const pixel=smooth.positions[i],x=(pixel%smooth.width+.5)*1280/smooth.width;
  const y=(Math.floor(pixel/smooth.width)+.5)*850/smooth.height;
  const center=scene.surface(x,y);
  for(let j=0;j<smooth.samples;j++){
    const [dx,dy]=m.MIRROR_PIXEL_SAMPLES[j];
    const surface=scene.surface(x+dx*1280/smooth.width,y+dy*850/smooth.height),p=(i*smooth.samples+j)*3;
    const direction=surface&&m.reflectedDirection(surface.view,surface.normal);
    if(surface)near(dot(direction,surface.normal),dot(surface.view,surface.normal));
    const hit=surface?.face===center.face&&scene.trace(surface.point,direction);
    if(hit){
      hits++;near(smooth.paths[p],hit.u);near(smooth.paths[p+1],hit.v);
      near(smooth.paths[p+2],m.dielectricFresnel(dot(surface.view,surface.normal))/smooth.samples);
    }else{blocked++;assert.equal(smooth.paths[p+2],0)}
    assert.ok(smooth.paths[p+2]>=0&&smooth.paths[p+2]<=1/smooth.samples);
  }
}
assert.ok(hits>100&&blocked>0);
// Roughness must spread source directions even without a reconstruction blur.
const spread=map=>{
  const p=map.positions.findIndex(pixel=>pixel%map.width===Math.floor(14*map.width/1280)
    &&Math.floor(pixel/map.width)===Math.floor(425*map.height/850));
  assert.ok(p>=0);
  let weight=0,sum=0,square=0;
  for(let j=0;j<map.samples;j++){
    const at=(p*map.samples+j)*3,w=map.paths[at+2],u=map.paths[at];
    weight+=w;sum+=u*w;square+=u*u*w;
  }
  assert.ok(weight>0);return square/weight-(sum/weight)**2;
};
assert.ok(spread(map)>spread(smooth)*25+1e-6);
console.log(JSON.stringify({rough:map.positions.length,smooth:smooth.positions.length,misses,hits,blocked}));
'''
    subprocess.run(['node', '-e', script, str(tmp_path/'terminalReflectionTransport.js')], check=True)


def test_curved_emitter_intersections_and_liner_join(tmp_path):
    frontend = Path(__file__).resolve().parents[1]
    subprocess.run([str(frontend/'node_modules/.bin/tsc'), 'src/features/terminal/terminalReflectionTransport.ts',
                    '--target', 'es2020', '--module', 'commonjs', '--skipLibCheck',
                    '--outDir', str(tmp_path)], cwd=frontend, check=True)
    script = r'''
const assert=require('assert/strict'),m=require(process.argv[1]);
const near=(a,b,e=1e-6)=>assert.ok(Math.abs(a-b)<e,`${a} != ${b}`);
for(const [w,h] of [[1280,850],[640,1000],[210,150],[60,60],[1200,70]])for(const curvature of [0,15,45,100]){
  const scene=m.reflectionScene(w,h,curvature,28),k=curvature/500,iw=w-56,ih=h-56;
  const rx=iw/2*(1+scene.depth/scene.camera),ry=ih/2*(1+scene.depth/scene.camera);
  const center=scene.trace([0,0,0],[0,0,-1]);
  near(center.distance,scene.depth-scene.bulge);near(center.u,.5);near(center.v,.5);
  for(const u of [-.8,0,.8])for(const v of [-.6,0,.6]){
    const x=w/2+u*iw/2,y=h/2+v*ih/2,p=scene.screenPoint(x,y);
    near(p[2],-scene.depth+scene.bulge*(1-(p[0]/rx)**2-(p[1]/ry)**2));
    near(p[0]/(1-p[2]/scene.camera)+w/2,x);
    near(p[1]/(1-p[2]/scene.camera)+h/2,y);
    const direction=[p[0],p[1],p[2]-scene.camera],norm=Math.hypot(...direction);
    const hit=scene.trace([0,0,scene.camera],direction.map(c=>c/norm));
    assert.ok(hit);near(hit.u,(u*(1+k*v*v)+1)/2);near(hit.v,(v*(1+k*u*u)+1)/2);
    near(hit.distance,norm,1e-5);
  }
  // Physical liner depth follows the curved screen at its projected edge.
  for(const v of [0,.4,.8]){
    const y=28+(v+1)*ih/2,x=28+iw/2*(1-1/(1+k*v*v));
    const edge=scene.screenPoint(x,y),wall=scene.surface(x-1e-5,y);
    assert.ok(wall);near(wall.point[2],edge[2],1e-3);
  }
  assert.equal(scene.trace([0,0,0],[0,0,1]),undefined);
  assert.equal(scene.trace([0,0,-200],[0,0,1]),undefined); // rear side
  if(curvature){
    // A grazing ray can hit the rising curved screen even without decreasing z.
    const grazing=scene.intersectScreen([-rx,0,-scene.depth+scene.bulge/2],[1,0,0]);
    assert.ok(grazing);near(grazing.point[0],-rx/Math.sqrt(2),1e-5);
    near(grazing.point[2],-scene.depth+scene.bulge/2);
    assert.equal(scene.intersectScreen([-rx,0,-scene.depth+scene.bulge],[1,0,0]),undefined);
    const normal=scene.intersectScreen([0,0,0],[1e-12,0,-1]);
    near(normal.distance,scene.depth-scene.bulge);
    if(Math.min(iw,ih)>50){
      const left=scene.surface(14,h/2),r=m.reflectedDirection(left.view,left.normal),hit=scene.trace(left.point,r);
      assert.ok(hit);
      const planeDistance=(-scene.depth-left.point[2])/r[2];
      assert.ok(Math.abs(hit.distance-planeDistance)>.01,'must intersect a curved surface, not its old plane');
    }
  }
}
'''
    subprocess.run(['node', '-e', script, str(tmp_path/'terminalReflectionTransport.js')], check=True)
