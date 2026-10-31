import { TERMINAL_REFLECTION_MATERIALS } from './terminalReflectionMaterial'
import type { TerminalReflectionMaterial } from './terminalReflectionMaterial'
import { bezelMaterialSampler } from './terminalBezelMaterial'
import { SCREEN_RECESS } from './terminalBezelGeometry'

// A fixed, centered viewer looks into a recessed dielectric liner. The emitting
// surface is a convex elliptic paraboloid, clipped by the existing CRT aperture.
// This is single-bounce geometric optics, not calibrated glass refraction.
// BRDF / visible GGX sampling: https://pbr-book.org/4ed/Reflection_Models
export type Vec3 = [number, number, number]
export const LOBE_SAMPLES = 24
export const TRANSPORT_SAMPLES = LOBE_SAMPLES * 2
export const LINER_ROUGHNESS = .32
// Finite pixel coverage, not material roughness. Every smooth sample still
// follows one ideal reflected direction at its own subpixel wall position.
export const MIRROR_PIXEL_SAMPLES = [[-.25,-.25],[.25,-.25],[-.25,.25],[.25,.25]] as const
const DIFFUSE_ALBEDO = .28
const dot = (a: Vec3, b: Vec3) => a[0]*b[0]+a[1]*b[1]+a[2]*b[2]
const mul = (a: Vec3, s: number): Vec3 => [a[0]*s,a[1]*s,a[2]*s]
const sub = (a: Vec3, b: Vec3): Vec3 => [a[0]-b[0],a[1]-b[1],a[2]-b[2]]
const cross = (a: Vec3, b: Vec3): Vec3 => [a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]]
const unit = (a: Vec3): Vec3 => mul(a,1/Math.max(1e-12,Math.hypot(...a)))
export const reflectedDirection = (view: Vec3, normal: Vec3): Vec3 => sub(mul(normal,2*dot(view,normal)),view)
export function dielectricFresnel(cosine: number, eta = 1.5) {
  const c = Math.max(0,Math.min(1,cosine)), t = Math.sqrt(1-(1-c*c)/(eta*eta))
  const parallel = (eta*c-t)/(eta*c+t), perpendicular = (c-eta*t)/(c+eta*t)
  return (parallel*parallel+perpendicular*perpendicular)/2
}
function lambda(c: number) {
  return (Math.sqrt(1+LINER_ROUGHNESS**2*(1-c*c)/Math.max(1e-12,c*c))-1)/2
}
function radicalInverse(n: number) {
  let result = 0, bit = .5
  while(n) { result += (n&1)*bit; bit *= .5; n >>>= 1 }
  return result
}
// Sample visible GGX microfacet normals. Invisible facets are never reflected.
function visibleNormal(view: Vec3, u: number, v: number): Vec3 {
  const vh = unit([LINER_ROUGHNESS*view[0],LINER_ROUGHNESS*view[1],view[2]])
  const t1: Vec3 = vh[2] < .99999 ? unit(cross([0,0,1],vh)) : [1,0,0]
  const t2 = cross(vh,t1), r = Math.sqrt(u), phi = 2*Math.PI*v
  const x = r*Math.cos(phi), s = (1+vh[2])/2
  const y = (1-s)*Math.sqrt(1-x*x)+s*r*Math.sin(phi)
  const z = Math.sqrt(Math.max(0,1-x*x-y*y))
  const h: Vec3 = [0,1,2].map(i=>t1[i]*x+t2[i]*y+vh[i]*z) as Vec3
  return unit([LINER_ROUGHNESS*h[0],LINER_ROUGHNESS*h[1],Math.max(0,h[2])])
}

export function reflectionScene(width: number, height: number, curvature: number, inset = SCREEN_RECESS) {
  const k = Math.max(0,Math.min(100,curvature))/500, iw = width-2*inset, ih = height-2*inset
  // Projected inset and physical depth are distinct. This slope represents a
  // deep plastic liner, with the aperture's perspective projection kept fixed.
  const depth = inset*2.5, camera = Math.max(width,height)*1.8
  // Tiny split panes must not put the glass edge beyond the camera's horizon.
  const bulge = Math.min(depth*Math.min(.45,2*k),Math.min(iw,ih)*.1)
  const rx = iw/2*(1+depth/camera), ry = ih/2*(1+depth/camera)
  const ax = bulge/(rx*rx), ay = bulge/(ry*ry)
  const edgeScale = bulge/(1+depth/camera)**2, cameraDepth = camera+depth-bulge
  const gapX = (y: number) => inset+iw/2*(1-1/(1+k*(2*(y-inset)/ih-1)**2))
  const gapY = (x: number) => inset+ih/2*(1-1/(1+k*(2*(x-inset)/iw-1)**2))
  const fractions = (x: number,y: number) => [y/gapY(x),(width-x)/gapX(y),(height-y)/gapY(x),x/gapX(y)]
  const world = (x: number,y: number,z: number): Vec3 => [(x-width/2)*(1-z/camera),(y-height/2)*(1-z/camera),z]
  const project = (p: Vec3) => [p[0]/(1-p[2]/camera)+width/2,p[1]/(1-p[2]/camera)+height/2]
  const screenHeight = (x: number,y: number) => -depth+bulge-ax*x*x-ay*y*y
  const intersectScreen = (point: Vec3,direction: Vec3) => {
    // Substitute p+t*d into z = -depth+bulge-ax*x²-ay*y². The stable
    // quadratic form avoids cancellation for near-normal and grazing rays.
    const a=ax*direction[0]**2+ay*direction[1]**2
    const b=direction[2]+2*(ax*point[0]*direction[0]+ay*point[1]*direction[1])
    const c=point[2]-screenHeight(point[0],point[1])
    let distance: number
    if(a<1e-16) {
      if(b>=-1e-12) return undefined
      distance=-c/b
    } else {
      const discriminant=b*b-4*a*c
      const tangentError=16*Number.EPSILON*(b*b+4*Math.abs(a*c))
      if(discriminant<=tangentError) return undefined // tangencies carry no front-facing emission
      const root=Math.sqrt(discriminant),q=-.5*(b+(b<0?-root:root))
      distance=Math.min(q/a,c/q)
    }
    if(!Number.isFinite(distance)||distance<=1e-7) return undefined
    const hit: Vec3=[point[0]+distance*direction[0],point[1]+distance*direction[1],point[2]+distance*direction[2]]
    // The terminal emits toward the observer. A ray hitting its rear is not a
    // valid source, including the second root of the paraboloid intersection.
    if(dot([2*ax*hit[0],2*ay*hit[1],1],direction)>=-1e-10) return undefined
    return {point:hit,distance}
  }
  const screenPoint = (x: number,y: number) => intersectScreen([0,0,camera],[x-width/2,y-height/2,-camera])!.point
  const wallDepth = (x: number,y: number) => {
    if(!bulge) return depth
    // Follow a projected radial line to the SAME curved aperture edge as the
    // SVG. Both walls at a mitre use this unique boundary depth, avoiding gaps.
    const u=2*(x-inset)/iw-1,v=2*(y-inset)/ih-1
    const major=Math.max(Math.abs(u),Math.abs(v)),minor=Math.min(Math.abs(u),Math.abs(v))
    if(major<1e-12) return depth-bulge
    const ratio=minor/major,c=k*ratio*ratio
    let t=1
    for(let i=0;i<4;i++) t-=(t+c*t*t*t-1)/(1+3*c*t*t)
    // Solve the camera ray's quadratic directly, without allocating vectors
    // for every visibility sample. This is the same surface as screenPoint().
    const a=edgeScale*t*t*(1+ratio*ratio)
    return 2*camera*cameraDepth/(camera+Math.sqrt(camera*camera-4*a*cameraDepth))-camera
  }
  const surface = (x: number,y: number) => {
    const f = fractions(x,y), t = Math.min(...f), face = f.indexOf(t)
    if(t<0 || t>=1) return undefined
    const point = world(x,y,-wallDepth(x,y)*t)
    const step = .1
    const px = world(x+step,y,-wallDepth(x+step,y)*fractions(x+step,y)[face])
    const py = world(x,y+step,-wallDepth(x,y+step)*fractions(x,y+step)[face])
    const normal = unit(cross(sub(px,point),sub(py,point)))
    return {point,normal,view:unit(sub([0,0,camera],point)),face}
  }
  const trace = (point: Vec3,direction: Vec3) => {
    const intersection=intersectScreen(point,direction)
    if(!intersection) return undefined
    const {point:hit,distance}=intersection
    const [x,y] = project(hit), u = 2*(x-inset)/iw-1, v = 2*(y-inset)/ih-1
    const su = u*(1+k*v*v), sv = v*(1+k*u*u)
    if(Math.abs(su)>=1 || Math.abs(sv)>=1) return undefined
    // A ray cannot travel through an adjacent liner wall to reach the screen.
    for(const t of [.0625,.25,.5,.75]) {
      const p = point.map((c,i)=>c+t*distance*direction[i]) as Vec3
      const [cx,cy] = project(p)
      const fraction=Math.min(...fractions(cx,cy))
      if(fraction<1 && fraction < -p[2]/wallDepth(cx,cy)-1e-3) return undefined
    }
    return {u:(su+1)/2,v:(sv+1)/2,distance}
  }
  return {surface,trace,depth,camera,bulge,screenPoint,screenHeight,intersectScreen}
}

export type ReflectionTransport = {
  width: number; height: number; positions: Uint32Array; faces: Uint8Array
  // Sparse, deterministic paths: normalized source UV plus radiance throughput.
  paths: Float32Array
  base?: Uint8Array
  samples?: number; weightScale?: number; radius?: number; sigma?: number
}
const transportCache = new Map<string, ReflectionTransport>()
export function buildReflectionTransport(width: number,height: number,curvature: number,material: TerminalReflectionMaterial = 'rough',inset = SCREEN_RECESS): ReflectionTransport {
  const settings = TERMINAL_REFLECTION_MATERIALS[material]
  const key = `${width}:${height}:${curvature}:${material}:${inset}`
  const cached = transportCache.get(key)
  if (cached) { transportCache.delete(key); transportCache.set(key,cached); return cached }
  const scale = Math.min(1,settings.maxEdge/Math.max(width,height))
  const w = Math.ceil(width*scale), h = Math.ceil(height*scale)
  const scene = reflectionScene(width,height,curvature,inset)
  const positions: number[] = [], paths: number[] = []
  const faces = new Uint8Array(w*h), base = new Uint8Array(w*h*4)
  const materialColor = bezelMaterialSampler(width,height,curvature,inset)
  for(let row=0;row<h;row++) for(let col=0;col<w;col++) {
    const x=(col+.5)*width/w,y=(row+.5)*height/h
    const s=scene.surface(x,y)
    if(!s) continue
    const {point,normal,view}=s, nv=dot(normal,view)
    if(nv<=0) continue
    const tangent=unit(cross(Math.abs(normal[2])<.99?[0,0,1]:[0,1,0],normal)), bitangent=cross(normal,tangent)
    const localView: Vec3=[dot(view,tangent),dot(view,bitangent),nv]
    const toWorld=(v: Vec3): Vec3=>[0,1,2].map(i=>v[0]*tangent[i]+v[1]*bitangent[i]+v[2]*normal[i]) as Vec3
    positions.push(row*w+col)
    faces[row*w+col]=s.face+1
    base.set([...materialColor(s.face,x,y),255],(row*w+col)*4)
    if(material === 'smooth') {
      for(const [dx,dy] of MIRROR_PIXEL_SAMPLES) {
        const sample=scene.surface(x+dx*width/w,y+dy*height/h)
        const hit=sample?.face===s.face
          ? scene.trace(sample.point,reflectedDirection(sample.view,sample.normal)):undefined
        paths.push(hit?.u??0,hit?.v??0,hit&&sample?dielectricFresnel(dot(sample.normal,sample.view))/MIRROR_PIXEL_SAMPLES.length:0)
      }
      continue
    }
    for(let sample=0;sample<TRANSPORT_SAMPLES;sample++) {
      const i=sample%LOBE_SAMPLES,u=(i+.5)/LOBE_SAMPLES,v=radicalInverse(i)
      let direction: Vec3,weight: number
      if(sample<LOBE_SAMPLES) {
        const micro=visibleNormal(localView,u,v)
        direction=reflectedDirection(localView,micro)
        // f*cos/pdf for visible-normal GGX: F * G2/G1(view).
        weight=direction[2]>0 ? dielectricFresnel(dot(localView,micro))*(1+lambda(nv))
          /(1+lambda(nv)+lambda(direction[2]))/LOBE_SAMPLES : 0
      } else {
        direction=[Math.sqrt(u)*Math.cos(2*Math.PI*v),Math.sqrt(u)*Math.sin(2*Math.PI*v),Math.sqrt(1-u)]
        // Cosine-weighted Lambertian substrate, with interface transmission.
        weight=DIFFUSE_ALBEDO*(1-dielectricFresnel(nv))*(1-dielectricFresnel(direction[2]))/LOBE_SAMPLES
      }
      const hit=weight>0?scene.trace(point,toWorld(direction)):undefined
      paths.push(hit?.u??0,hit?.v??0,hit?weight:0)
    }
    // Misses remain zero. Never renormalize hits: the screen's solid angle,
    // occlusion and the BRDF determine how much light reaches this point.
    // Radiance along a specular ray does NOT receive an extra inverse-square loss.
  }
  const result = {width:w,height:h,faces,base,samples:settings.samples,weightScale:settings.weightScale,
    radius:settings.radius,sigma:settings.sigma,positions:new Uint32Array(positions),paths:new Float32Array(paths)}
  if(transportCache.size>=4) transportCache.delete(transportCache.keys().next().value!)
  transportCache.set(key,result)
  return result
}
