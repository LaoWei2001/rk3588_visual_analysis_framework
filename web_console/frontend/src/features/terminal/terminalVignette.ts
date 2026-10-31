import { useEffect } from 'react'
import type { RefObject } from 'react'

type VignetteMap = { canvas: HTMLCanvasElement; image: string }
const maps = new Map<string, VignetteMap>()
const MAP_EDGE = 384
const MAX_MAPS = 12

/** Art-directed optical approximation, not calibration of a particular CRT.
 * Work in source/glass coordinates, before the existing screen displacement.
 * The same transmission field shades the screen and its reflected light.
 */
export function terminalVignetteMap(width: number, height: number, strength: number, curvature: number): VignetteMap | undefined {
  if (!width || !height || strength <= 0) return undefined
  const scale = Math.min(1, MAP_EDGE / Math.max(width, height))
  const w = Math.max(1, Math.round(width * scale)), h = Math.max(1, Math.round(height * scale))
  const amount = Math.min(100, strength) / 100, curve = Math.max(0, Math.min(100, curvature)) / 100
  const key = `${w}:${h}:${amount}:${curve}`
  const cached = maps.get(key)
  if (cached) {
    maps.delete(key); maps.set(key, cached)
    return cached
  }
  const canvas = document.createElement('canvas')
  canvas.width = w; canvas.height = h
  const ctx = canvas.getContext('2d')!, pixels = ctx.createImageData(w, h)
  // Physical aspect keeps the lip's penumbra equally wide on all four edges.
  // It must not turn into a stretched elliptical ring on a wide/split display.
  const halfWidth = w / Math.min(w, h), halfHeight = h / Math.min(w, h)
  const diagonal = Math.hypot(halfWidth, halfHeight)
  const radius = .14 + curve * .10
  const slope = .32 + curve * .55
  const penumbra = .075 + curve * .035
  for (let row = 0; row < h; row++) {
    const v = (row + .5) / h * 2 - 1, y = Math.abs(v) * halfHeight
    for (let col = 0; col < w; col++) {
      const u = (col + .5) / w * 2 - 1, x = Math.abs(u) * halfWidth
      const radial = (x * x + y * y) / (diagonal * diagonal)
      // Longer oblique paths through curved face glass, plus gentle peripheral
      // emission falloff. The center stays clear with no fixed gradient start.
      const nx = slope * u * (1 + .25 * v * v) * halfWidth / diagonal
      const ny = slope * v * (1 + .25 * u * u) * halfHeight / diagonal
      const glassDensity = .65 * (Math.sqrt(1 + nx * nx + ny * ny) - 1)
      const emissionDensity = .18 * radial ** 1.7
      const cornerDensity = (.22 + .16 * curve) * (Math.abs(u * v) ** 3)
      // Distance to a rounded rectangular opening: broad penumbra and a narrow
      // contact shadow, rather than a circular vignette painted over the text.
      const qx = x - halfWidth + radius, qy = y - halfHeight + radius
      const distance = Math.max(0, radius - Math.hypot(Math.max(qx, 0), Math.max(qy, 0)) - Math.min(Math.max(qx, qy), 0))
      const lipDensity = .62 * Math.exp(-distance / penumbra) + .68 * Math.exp(-distance / .018)
      const transmission = Math.exp(-amount * (glassDensity + emissionDensity + cornerDensity + lipDensity))
      // Fixed sub-byte dither breaks up 8-bit contour bands; it never animates
      // or adds another snow layer. RGB stays neutral to preserve ANSI colors.
      let hash = Math.imul(col + 1, 374761393) ^ Math.imul(row + 1, 668265263)
      hash = Math.imul(hash ^ (hash >>> 13), 1274126177)
      const dither = ((hash >>> 0) / 0x100000000 - .5) * .8
      pixels.data[(row * w + col) * 4 + 3] = Math.round(Math.min(.88, 1 - transmission) * 255 + dither)
    }
  }
  ctx.putImageData(pixels, 0, 0)
  const result = { canvas, image: `url("${canvas.toDataURL()}")` }
  if (maps.size >= MAX_MAPS) maps.delete(maps.keys().next().value!)
  maps.set(key, result)
  return result
}

/** Update only on geometry/settings changes; no animation loop or GPU readback. */
export function useTerminalVignette(surfaceRef: RefObject<HTMLDivElement>, enabled: boolean, strength: number, curvature: number) {
  useEffect(() => {
    const surface = surfaceRef.current
    if (!surface || !enabled || !strength) return
    let frame = 0
    const paint = () => {
      frame = 0
      const map = terminalVignetteMap(surface.clientWidth, surface.clientHeight, strength, curvature)
      surface.style.setProperty('--crt-vignette-texture', map?.image ?? 'none')
    }
    const schedule = () => { if (!frame) frame = requestAnimationFrame(paint) }
    const observer = new ResizeObserver(schedule)
    observer.observe(surface); schedule()
    return () => {
      cancelAnimationFrame(frame); observer.disconnect()
      surface.style.removeProperty('--crt-vignette-texture')
    }
  }, [surfaceRef, enabled, strength, curvature])
}
