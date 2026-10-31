// Match xterm's luminance adjustment in the low-resolution reflection/history
// renderer too. Cache color pairs, never sample the DOM or read pixels per frame.
export const TERMINAL_MIN_CONTRAST = 4.5
type RGB = number[]
const channels = (css: string): RGB => css.startsWith('#')
  ? [1, 3, 5].map(i => parseInt(css.slice(i, i + 2), 16))
  : (css.match(/[\d.]+/g) ?? []).slice(0, 3).map(Number)
const luminance = (rgb: RGB) => rgb.reduce((sum, value, i) => {
  const n = value / 255
  return sum + (n <= .04045 ? n / 12.92 : ((n + .055) / 1.055) ** 2.4) * [.2126, .7152, .0722][i]
}, 0)
const contrast = (a: number, b: number) => (Math.max(a, b) + .05) / (Math.min(a, b) + .05)

export function createTerminalContrast() {
  const cache = new Map<string, string>()
  return (foreground: string, background: string, dim: boolean, code: number): string => {
    // Box/block and powerline glyphs intentionally serve as colored graphics.
    if ((code >= 0x2500 && code <= 0x259f) || (code >= 0xe0b0 && code <= 0xe0bf)) return foreground
    const key = `${foreground}:${background}:${dim}`
    const cached = cache.get(key)
    if (cached) return cached
    const fg = channels(foreground), bg = channels(background)
    const target = TERMINAL_MIN_CONTRAST / (dim ? 2 : 1), bgL = luminance(bg)
    let adjusted = fg
    if (contrast(luminance(fg), bgL) < target) {
      const adjust = (lighten: boolean) => {
        let color = fg.slice()
        while (contrast(luminance(color), bgL) < target && color.some(n => lighten ? n < 255 : n > 0)) {
          color = color.map(n => lighten ? n + Math.ceil((255 - n) * .1) : n - Math.ceil(n * .1))
        }
        return color
      }
      const lighten = luminance(fg) >= bgL
      adjusted = adjust(lighten)
      if (contrast(luminance(adjusted), bgL) < target) {
        const alternative = adjust(!lighten)
        if (contrast(luminance(alternative), bgL) > contrast(luminance(adjusted), bgL)) adjusted = alternative
      }
    }
    const result = adjusted === fg ? foreground : `rgb(${adjusted.join(',')})`
    if (cache.size >= 512) cache.clear()
    cache.set(key, result)
    return result
  }
}
