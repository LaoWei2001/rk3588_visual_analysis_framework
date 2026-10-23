import type { TerminalAppearance } from './terminalAppearance'

// Shared by the visible CSS layers and the light sampled for reflections.
export const VIGNETTE_START = .48
export const vignetteOpacity = (strength: number) => strength / 100 * .8
export const scanlineOpacity = (strength: number) => strength / 100 * .5
export const afterglowOpacity = (strength: number) => strength > 0 ? .25 + strength / 100 * .45 : 0
export type ScreenOptics = Pick<TerminalAppearance, 'vignette' | 'scanlines' | 'glow' | 'bloom' | 'pixelGrid' | 'pixelSize'>

const DOT_SCALE = 8
const dotTiles = new Map<number, { canvas: HTMLCanvasElement; url: string }>()

/** A fixed phosphor lattice. Dark gaps modulate actual emitted light; no fake lit dots. */
function dotTile(size: number) {
  const cached = dotTiles.get(size)
  if (cached) return cached
  const canvas = document.createElement('canvas')
  canvas.width = canvas.height = size * DOT_SCALE
  const ctx = canvas.getContext('2d')!, center = canvas.width / 2
  const gradient = ctx.createRadialGradient(center, center, 0, center, center, canvas.width * .49)
  gradient.addColorStop(0, 'transparent')
  gradient.addColorStop(.55, 'transparent')
  gradient.addColorStop(1, 'rgba(0,0,0,.94)')
  ctx.fillStyle = gradient; ctx.fillRect(0, 0, canvas.width, canvas.height)
  const tile = { canvas, url: `url("${canvas.toDataURL()}")` }
  dotTiles.set(size, tile)
  return tile
}
export const terminalDotTexture = (size: number) => dotTile(size).url

/** Light lost at the source pixel, before any mirror mapping or joint clipping. */
export function paintScreenAttenuation(canvas: HTMLCanvasElement, width: number, height: number, optics: ScreenOptics) {
  const ctx = canvas.getContext('2d')!
  ctx.resetTransform()
  ctx.clearRect(0, 0, canvas.width, canvas.height)
  // CSS radial-gradient(ellipse at center) defaults to farthest-corner, not sides.
  ctx.setTransform(canvas.width / Math.SQRT2, 0, 0, canvas.height / Math.SQRT2, canvas.width / 2, canvas.height / 2)
  const gradient = ctx.createRadialGradient(0, 0, 0, 0, 0, 1)
  gradient.addColorStop(0, 'transparent')
  gradient.addColorStop(VIGNETTE_START, 'transparent')
  gradient.addColorStop(1, `rgba(0,0,0,${vignetteOpacity(optics.vignette)})`)
  ctx.fillStyle = gradient
  ctx.fillRect(-1, -1, 2, 2)
  ctx.setTransform(canvas.width / width, 0, 0, canvas.height / height, 0, 0)
  ctx.fillStyle = `rgba(0,0,0,${scanlineOpacity(optics.scanlines)})`
  for (let y = 2; y < height; y += 3) ctx.fillRect(0, y, width, 1)
  if (optics.pixelGrid > 0) {
    const pattern = ctx.createPattern(dotTile(optics.pixelSize).canvas, 'repeat')!
    pattern.setTransform(new DOMMatrix().scale(1 / DOT_SCALE))
    ctx.globalAlpha = optics.pixelGrid / 100
    ctx.fillStyle = pattern; ctx.fillRect(0, 0, width, height)
    ctx.globalAlpha = 1
  }
  ctx.resetTransform()
}


/** Same emissive halos for DOM text and reflected light; ANSI colors stay intact. */
export function terminalLightHalos(glow: number, bloom: number, rgb: string) {
  const g = glow / 100, b = bloom / 100
  return [
    ...(g ? [{ radius: 1 + g * 2, color: 'currentColor' },
      { radius: 3 + g * 9, color: `rgba(${rgb},${g * .55})` }] : []),
    ...(b ? [{ radius: 4 + b * 7, color: `rgba(${rgb},${b * .65})` },
      { radius: 12 + b * 16, color: `rgba(${rgb},${b * .45})` }] : []),
  ]
}
