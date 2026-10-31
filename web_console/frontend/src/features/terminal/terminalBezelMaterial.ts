import { SCREEN_RECESS } from './terminalBezelGeometry'

/** Shared by the visible SVG material and linear-light reflection composition. */
export function bezelMaterialGradients(width: number, height: number, amount: number, inset = SCREEN_RECESS) {
  const depthX = Math.max(1, inset + (width - inset * 2) / 2 * amount / (1 + amount))
  const depthY = Math.max(1, inset + (height - inset * 2) / 2 * amount / (1 + amount))
  return [
    { face: 'top', x1: 0, y1: 0, x2: 0, y2: depthY, outer: '#66685b', inner: '#25281f' },
    { face: 'right', x1: width, y1: 0, x2: width - depthX, y2: 0, outer: '#55594c', inner: '#20251c' },
    { face: 'bottom', x1: 0, y1: height, x2: 0, y2: height - depthY, outer: '#777768', inner: '#35382d' },
    { face: 'left', x1: 0, y1: 0, x2: depthX, y2: 0, outer: '#4b5043', inner: '#1c211a' },
  ]
}
export function bezelMaterialSampler(width: number, height: number, curvature: number, inset = SCREEN_RECESS) {
  const gradients = bezelMaterialGradients(width,height,curvature/500,inset).map(g=>({
    ...g, a: [1,3,5].map(i=>parseInt(g.outer.slice(i,i+2),16)), b: [1,3,5].map(i=>parseInt(g.inner.slice(i,i+2),16)),
  }))
  return (face: number,x: number,y: number) => {
    const g=gradients[face],dx=g.x2-g.x1,dy=g.y2-g.y1
    const t=Math.max(0,Math.min(1,(((x-g.x1)*dx+(y-g.y1)*dy)/(dx*dx+dy*dy)-.12)/.88))
    return g.a.map((v,i)=>Math.round(v+(g.b[i]-v)*t))
  }
}
