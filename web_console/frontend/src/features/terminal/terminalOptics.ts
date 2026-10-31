import type { TerminalAppearance } from './terminalAppearance'
import { terminalVignetteMap } from './terminalVignette'

// Shared by the visible CSS layers and the light sampled for reflections.
export const scanlineOpacity = (strength: number) => strength / 100 * .5
export const afterglowOpacity = (strength: number) => strength > 0 ? .25 + strength / 100 * .45 : 0
export type ScreenOptics = Pick<TerminalAppearance, 'vignette' | 'curvature' | 'scanlines' | 'glow' | 'bloom' | 'pixelGrid' | 'pixelSize'>

const DOT_SCALE = 8
const dotTiles = new Map<number, { canvas: HTMLCanvasElement; url: string; meanLoss: number }>()

/** Staggered phosphor grains with a broad emitting core and narrow dark seams.
 * Only existing screen light is modulated; unlit areas never acquire fake dots.
 */
function dotTile(size: number) {
  const cached = dotTiles.get(size)
  if (cached) return cached
  const canvas = document.createElement('canvas')
  const pitch = size * DOT_SCALE
  canvas.width = canvas.height = pitch * 2
  const ctx = canvas.getContext('2d')!, pixels = ctx.createImageData(canvas.width, canvas.height)
  let totalAlpha = 0
  for (let y = 0; y < canvas.height; y++) {
    const shift = Math.floor(y / pitch) % 2 * .5
    const dy = Math.abs((y + .5) / pitch % 1 - .5) / .485
    for (let x = 0; x < canvas.width; x++) {
      const dx = Math.abs(((x + .5) / pitch + shift) % 1 - .5) / .495
      // Rounded grains, not a broad radial dimmer: most of each cell stays lit.
      const radius = Math.cbrt(dx ** 3 + dy ** 3)
      const edge = Math.max(0, Math.min(1, (radius - .82) / .20))
      const alpha = Math.round(.82 * edge * edge * (3 - 2 * edge) * 255)
      pixels.data[(y * canvas.width + x) * 4 + 3] = alpha
      totalAlpha += alpha
    }
  }
  ctx.putImageData(pixels, 0, 0)
  const tile = { canvas, url: `url("${canvas.toDataURL()}")`, meanLoss: totalAlpha / (255 * canvas.width * canvas.height) }
  dotTiles.set(size, tile)
  return tile
}
export const terminalDotTexture = (size: number) => dotTile(size).url
// Normalize average emitted light so the strength control mainly changes grain
// contrast, not brightness. Peak-white light still has the display's normal cap.
export const terminalDotGain = (size: number, strength: number) => 1 / (1 - strength / 100 * dotTile(size).meanLoss)

/** Light lost at the source pixel, before any mirror mapping or joint clipping. */
export function paintScreenAttenuation(canvas: HTMLCanvasElement, width: number, height: number, optics: ScreenOptics) {
  const ctx = canvas.getContext('2d')!
  ctx.resetTransform()
  ctx.clearRect(0, 0, canvas.width, canvas.height)
  const vignette = terminalVignetteMap(width, height, optics.vignette, optics.curvature)
  if (vignette) ctx.drawImage(vignette.canvas, 0, 0, canvas.width, canvas.height)
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
