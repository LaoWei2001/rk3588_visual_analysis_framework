// Source-coordinate horizontal displacement shared by the SVG, mouse correction,
// and reflection sampler. Values are quantized to the actual displacement texture.
export const SIGNAL_ROWS = 128
export const SIGNAL_EVENT = 'terminal-signal-change'
export const FONT_EVENT = 'terminal-font-ready'
export type SignalState = { offsets: Float32Array; rgbShift: number; jitterX: number; jitterY: number; frame: number; filterId: string }
const signals = new WeakMap<HTMLElement, SignalState>()
export const getTerminalSignal = (surface: HTMLElement) => signals.get(surface)
export function setTerminalSignal(surface: HTMLElement, state?: SignalState) {
  if (state) signals.set(surface, state)
  else signals.delete(surface)
  surface.dispatchEvent(new Event(SIGNAL_EVENT))
}
export function signalOffset(surface: HTMLElement, y: number) {
  const state = signals.get(surface)
  if (!state) return 0
  const row = Math.max(0, Math.min(SIGNAL_ROWS - 1, y / surface.clientHeight * SIGNAL_ROWS - .5))
  const i = Math.floor(row), f = row - i
  return state.offsets[i] * (1 - f) + state.offsets[Math.min(i + 1, SIGNAL_ROWS - 1)] * f
}

/** Reusable low-resolution buffers; no pixel readbacks or per-frame allocations. */
export function createSignalPainter() {
  const warped = document.createElement('canvas'), channels = document.createElement('canvas')
  const warp = warped.getContext('2d')!, channel = channels.getContext('2d')!
  const colors = new WeakMap<HTMLCanvasElement, { canvas: HTMLCanvasElement; version: number; shift: number; filterId: string }>()
  return (source: HTMLCanvasElement, surface: HTMLElement, version: number) => {
    const signal = getTerminalSignal(surface)
    if (!signal) return source
    const w = source.width, h = source.height, scale = w / surface.clientWidth
    let light = source
    if (signal.rgbShift) {
      let cached = colors.get(source)
      if (!cached) {
        cached = { canvas: document.createElement('canvas'), version: -1, shift: -1, filterId: '' }
        colors.set(source, cached)
      }
      if (cached.version !== version || cached.shift !== signal.rgbShift || cached.filterId !== signal.filterId
        || cached.canvas.width !== w || cached.canvas.height !== h) {
        for (const canvas of [channels, cached.canvas]) {
          if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h }
        }
        const output = cached.canvas.getContext('2d')!
        output.clearRect(0, 0, w, h); output.globalCompositeOperation = 'lighter'
        for (const [name, direction] of [['red', -1], ['green', 0], ['blue', 1]] as const) {
          channel.clearRect(0, 0, w, h)
          // Preserve alpha at antialiased edges, without introducing false colors.
          channel.filter = `url(#${signal.filterId}-${name}-channel)`
          channel.drawImage(source, 0, 0); channel.filter = 'none'
          output.drawImage(channels, direction * signal.rgbShift * scale, 0)
        }
        output.globalCompositeOperation = 'source-over'
        cached.version = version; cached.shift = signal.rgbShift; cached.filterId = signal.filterId
      }
      light = cached.canvas
    }
    // Horizontal channel offsets commute with the per-line horizontal wave.
    // Cache color separation when only jitter/sync moves; never freeze the motion.
    const hasWave = signal.offsets.some(value => value)
    if (!hasWave && !signal.jitterX && !signal.jitterY) return light
    if (warped.width !== w || warped.height !== h) { warped.width = w; warped.height = h }
    warp.clearRect(0, 0, w, h)
    if (!hasWave) warp.drawImage(light, signal.jitterX * scale, signal.jitterY * scale)
    else {
      for (let y = 0; y < h; y += 2) {
        const height = Math.min(2, h - y)
        const dx = signalOffset(surface, (y + height / 2) / h * surface.clientHeight) * scale
        warp.drawImage(light, 0, y, w, height, -dx + signal.jitterX * scale, y + signal.jitterY * scale, w, height)
      }
    }
    return warped
  }
}

// Encode the 1x128 RGB displacement image entirely on the CPU. Reading a tiny
// GPU-backed canvas with toDataURL can stall all pending terminal drawing.
const crcTable = Uint32Array.from({ length: 256 }, (_, n) => {
  let crc = n
  for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ (crc & 1 ? 0xedb88320 : 0)
  return crc >>> 0
})
export function signalMapImage(red: Uint8Array): string {
  const raw = new Uint8Array(red.length * 4)
  let a = 1, b = 0
  for (let row = 0; row < red.length; row++) {
    raw.set([0, red[row], 128, 128], row * 4) // PNG filter 0, RGB
  }
  for (const byte of raw) { a = (a + byte) % 65521; b = (b + a) % 65521 }
  // One uncompressed DEFLATE block: small, exact and free of canvas readbacks.
  const zlib = new Uint8Array(raw.length + 11), zview = new DataView(zlib.buffer)
  zlib.set([0x78, 0x01, 0x01, raw.length & 255, raw.length >>> 8, (~raw.length) & 255, ((~raw.length) >>> 8) & 255])
  zlib.set(raw, 7); zview.setUint32(zlib.length - 4, ((b << 16) | a) >>> 0)
  const chunk = (name: string, data: Uint8Array) => {
    const bytes = new Uint8Array(data.length + 12), view = new DataView(bytes.buffer)
    view.setUint32(0, data.length)
    for (let i = 0; i < 4; i++) bytes[i + 4] = name.charCodeAt(i)
    bytes.set(data, 8)
    let crc = 0xffffffff
    for (let i = 4; i < bytes.length - 4; i++) crc = crcTable[(crc ^ bytes[i]) & 255] ^ (crc >>> 8)
    view.setUint32(bytes.length - 4, (crc ^ 0xffffffff) >>> 0)
    return bytes
  }
  const header = new Uint8Array(13), hview = new DataView(header.buffer)
  hview.setUint32(0, 1); hview.setUint32(4, red.length); header[8] = 8; header[9] = 2
  const parts = [new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', header), chunk('IDAT', zlib), chunk('IEND', new Uint8Array())]
  return 'data:image/png;base64,' + btoa(parts.map(part => String.fromCharCode(...part)).join(''))
}
