export const linearLight = (v: number) => v <= .04045 ? v/12.92 : ((v+.055)/1.055)**2.4
export const displayLight = (v: number) => v <= .0031308 ? v*12.92 : 1.055*v**(1/2.4)-.055

/** Encode only the difference between base and base+reflected radiance.
 * Normal browser source-over then reproduces the linear-light sum instead of
 * laying a white, gamma-encoded glow over the material. Returns premultiplied RGBA.
 */
export function reflectionOverlay(base: number[], radiance: number[], strength: number) {
  const delta=base.map((b,i)=>Math.max(0,Math.min(1,displayLight(linearLight(b)+radiance[i]*strength))-b))
  const alpha=Math.max(...delta.map((d,i)=>d/Math.max(1e-6,1-base[i])))
  return [...delta.map((d,i)=>d+base[i]*alpha),alpha]
}
