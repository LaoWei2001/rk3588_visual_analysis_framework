import { useEffect, useRef } from 'react'
import type { RefObject } from 'react'
import type { IBufferCell, ITheme } from '@xterm/xterm'
import { getSession } from './terminalSession'
import { TERMINAL_PROFILES, terminalAppearanceOptions } from './terminalAppearance'
import type { TerminalAppearance } from './terminalAppearance'
import { BEZEL_JOINT_WIDTH, SCREEN_RECESS, bezelFaces, bezelJoints } from './terminalBezelGeometry'
import type { BezelFace } from './terminalBezelGeometry'
import { createSignalPainter, getTerminalSignal, SIGNAL_EVENT, FONT_EVENT } from './terminalSignal'
import { createReflectionRaster } from './terminalReflectionRaster'
import type { ReflectionSlice } from './terminalReflectionRaster'
import { afterglowOpacity, terminalLightHalos, paintScreenAttenuation } from './terminalOptics'

// Rendering ideas: cool-retro-term's terminal_dynamic/static.frag and BurnInEffect.qml.
// Independent Canvas implementation: cached noise, mirrored edge samples, decaying history.
let snowTexture: string | undefined
export function terminalSnowTexture(): string {
  if (snowTexture) return snowTexture
  const canvas = document.createElement('canvas')
  canvas.width = canvas.height = 256
  const context = canvas.getContext('2d')!
  const pixels = context.createImageData(256, 256)
  let seed = 0x3588abcd
  for (let i = 0; i < pixels.data.length; i += 4) {
    seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5
    const value = (seed >>> 0) / 0xffffffff
    pixels.data[i] = pixels.data[i + 1] = pixels.data[i + 2] = 255
    pixels.data[i + 3] = Math.round(value * value * 255)
  }
  context.putImageData(pixels, 0, 0)
  snowTexture = `url("${canvas.toDataURL()}")`
  return snowTexture
}

function palette(theme: ITheme) {
  const base = [theme.black, theme.red, theme.green, theme.yellow, theme.blue, theme.magenta, theme.cyan, theme.white,
    theme.brightBlack, theme.brightRed, theme.brightGreen, theme.brightYellow,
    theme.brightBlue, theme.brightMagenta, theme.brightCyan, theme.brightWhite]
  return (index: number): string => {
    if (index < 16) return base[index] ?? theme.foreground ?? '#ffbc69'
    if (index >= 232) { const gray = 8 + (index - 232) * 10; return `rgb(${gray},${gray},${gray})` }
    const n = index - 16, level = [0, 95, 135, 175, 215, 255]
    return `rgb(${level[Math.floor(n / 36)]},${level[Math.floor(n / 6) % 6]},${level[n % 6]})`
  }
}

/** Cache the source-coordinate mapping; moving text never rebuilds the geometry. */
function createBezelReflector() {
  type Slice = ReflectionSlice
  const raster = createReflectionRaster()
  let key = '', horizontal: Slice[] = [], vertical: Slice[] = []
  let faces: Record<BezelFace, [number, number][]> | undefined
  let joints: ReturnType<typeof bezelJoints> = []
  const reflect = (target: CanvasRenderingContext2D, source: HTMLCanvasElement, curvature: number, pixelScale: number) => {
    const w = target.canvas.width, h = target.canvas.height, sw = source.width, sh = source.height
    target.clearRect(0, 0, w, h)
    const k = curvature / 500, inset = SCREEN_RECESS * pixelScale
    if (!k) return
    const nextKey = `${w}:${h}:${sw}:${sh}:${k}:${inset}`
    if (key !== nextKey) {
      key = nextKey; horizontal = []; vertical = []
      faces = bezelFaces(w, h, k, inset); joints = bezelJoints(w, h, k, inset)
      const iw = w - inset * 2, ih = h - inset * 2
      const tangentStep = 3
      // Invert the same cross-axis warp as the glass, at the mirrored sample.
      // Subdivide only where cross-axis drift needs it (at most three bands). Samples
      // outside the original screen are clipped, never folded back into text.
      for (let x = 0; x < w; x += tangentStep) {
        const dw = Math.min(tangentStep, w - x), n = 2 * (x + dw / 2 - inset) / iw - 1
        const gap = inset + ih / 2 * (1 - 1 / (1 + k * n * n))
        const scaleY = sh / ih * (1 + k * n * n)
        const bands = Math.min(3, Math.max(1, Math.ceil(Math.abs(n) * k * gap * sw / (ih * 4))))
        for (let band = 0; band < bands; band++) {
          const d = gap * band / bands, dh = gap / bands
          const v = 2 * (gap + d + dh / 2 - inset) / ih - 1
          const scaleX = sw / iw * (1 + k * v * v)
          const sx = (1 + (2 * (x - inset) / iw - 1) * (1 + k * v * v)) * sw / 2
          const sy = d * scaleY, width = dw * scaleX, height = dh * scaleY
          if (sx + width <= 0 || sx >= sw || sy >= sh) continue
          horizontal.push([sx, sy, width, height, x, -gap + d, dw, dh])
          horizontal.push([sx, sh - sy - height, width, height, x, -h + gap - d - dh, dw, dh])
        }
      }
      for (let y = 0; y < h; y += tangentStep) {
        const dh = Math.min(tangentStep, h - y), n = 2 * (y + dh / 2 - inset) / ih - 1
        const gap = inset + iw / 2 * (1 - 1 / (1 + k * n * n))
        const scaleX = sw / iw * (1 + k * n * n)
        const bands = Math.min(3, Math.max(1, Math.ceil(Math.abs(n) * k * gap * sh / (iw * 4))))
        for (let band = 0; band < bands; band++) {
          const d = gap * band / bands, dw = gap / bands
          const u = 2 * (gap + d + dw / 2 - inset) / iw - 1
          const scaleY = sh / ih * (1 + k * u * u)
          const sy = (1 + (2 * (y - inset) / ih - 1) * (1 + k * u * u)) * sh / 2
          const sx = d * scaleX, width = dw * scaleX, height = dh * scaleY
          if (sy + height <= 0 || sy >= sh || sx >= sw) continue
          vertical.push([sx, sy, width, height, -gap + d, y, dw, dh])
          vertical.push([sw - sx - width, sy, width, height, -w + gap - d - dw, y, dw, dh])
        }
      }
      raster?.configure([horizontal, vertical], w, h, sw, sh)
    }
    const clipFaces = (names: BezelFace[]) => {
      target.beginPath()
      for (const name of names) {
        const [first, ...rest] = faces![name]
        target.moveTo(...first)
        for (const point of rest) target.lineTo(...point)
        target.closePath()
      }
      target.clip()
    }
    const batched = raster?.upload(source)
    target.save(); clipFaces(['top', 'bottom'])
    if (batched) target.drawImage(raster!.draw(0), 0, 0)
    else {
      target.setTransform(1, 0, 0, -1, 0, 0)
      for (const slice of horizontal) target.drawImage(source, ...slice)
    }
    target.restore()
    target.save(); clipFaces(['left', 'right'])
    if (batched) target.drawImage(raster!.draw(1), 0, 0)
    else {
      target.setTransform(-1, 0, 0, 1, 0, 0)
      for (const slice of vertical) target.drawImage(source, ...slice)
    }
    target.restore()
    target.save()
    target.globalCompositeOperation = 'destination-out'
    target.lineWidth = BEZEL_JOINT_WIDTH * pixelScale
    target.beginPath()
    for (const [outer, inner] of joints) { target.moveTo(...outer); target.lineTo(...inner) }
    target.stroke(); target.restore()
  }
  reflect.dispose = () => raster?.dispose()
  return reflect
}

export function useTerminalPhosphor(id: string, surfaceRef: RefObject<HTMLDivElement>,
  historyRef: RefObject<HTMLCanvasElement>, reflectionRef: RefObject<HTMLCanvasElement>,
  appearance: TerminalAppearance, fontSize: number) {
  const { enabled, profile, afterglow, reflection, curvature } = appearance
  const optics = useRef({ vignette: appearance.vignette, scanlines: appearance.scanlines, glow: appearance.glow, bloom: appearance.bloom, pixelGrid: appearance.pixelGrid, pixelSize: appearance.pixelSize })
  const refreshOptics = useRef<() => void>()
  useEffect(() => {
    const term = getSession(id)?.term
    const surface = surfaceRef.current, history = historyRef.current, reflected = reflectionRef.current
    if (!term || !surface || !history || !reflected) return
    const ghost = history.getContext('2d'), frame = reflected.getContext('2d')
    if (!ghost || !frame) return
    ghost.clearRect(0, 0, history.width, history.height)
    frame.clearRect(0, 0, reflected.width, reflected.height)
    if (!enabled) return

    const current = document.createElement('canvas'), reflectionSource = document.createElement('canvas')
    const ink = current.getContext('2d')!, light = reflectionSource.getContext('2d')!
    const attenuation = document.createElement('canvas')
    const reflectedAttenuation = document.createElement('canvas')
    const reflectedMask = reflectedAttenuation.getContext('2d')!
    let opticsDirty = true, capturedHalos = ''
    const reflectionStill = document.createElement('canvas'), reflectionHistory = document.createElement('canvas')
    const still = reflectionStill.getContext('2d')!, echo = reflectionHistory.getContext('2d')!
    let reflectionCached = false, signalDirty = true, sourceVersion = 0
    const paintSignal = createSignalPainter()
    const reflectFrame = createBezelReflector()
    const theme = terminalAppearanceOptions(appearance).theme!
    const indexedColor = palette(theme)
    const reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)')
    const bezel = surface.parentElement!
    const content = surface.querySelector<HTMLElement>('.term-phosphor-content')!
    let disposed = false, intersecting = true, measuring = true
    let timer: ReturnType<typeof setTimeout> | undefined
    let rowKeys: string[] = [], dirtyStart = 0, dirtyEnd = Infinity
    let lastTick = 0, lastDecay = 0, expires = 0
    let width = 0, height = 0, scale = 1, cellWidth = 0, cellHeight = 0, left = 0, top = 0
    type CursorImage = { x: number; y: number; w: number; h: number; outline: boolean; lit: boolean }
    let cursor: CursorImage | undefined, cursorKey = '', nextBlink = Infinity
    const fadeTime = 160 + afterglow / 100 * 1440
    const visible = () => !disposed && !document.hidden && intersecting
    const clearHistory = () => { ghost.clearRect(0, 0, history.width, history.height); expires = 0 }
    const fgColor = (cell: IBufferCell, background: boolean) => {
      const rgb = background ? cell.isBgRGB() : cell.isFgRGB()
      const indexed = background ? cell.isBgPalette() : cell.isFgPalette()
      const color = background ? cell.getBgColor() : cell.getFgColor()
      if (rgb) return `#${color.toString(16).padStart(6, '0')}`
      if (indexed) return indexedColor(color + (!background && cell.isBold() && color < 8 ? 8 : 0))
      return (background ? theme.background : theme.foreground) ?? '#ffbc69'
    }

    const measure = () => {
      const row = surface.querySelector('.xterm-rows > div')
      if (!row) return false
      const bounds = content.getBoundingClientRect(), cells = row.getBoundingClientRect()
      width = surface.clientWidth; height = surface.clientHeight
      if (!width || !height || !cells.width || !cells.height) return false
      scale = Math.min(1, 1100 / width)
      left = Math.round((cells.left - bounds.left) * 64) / 64; top = Math.round((cells.top - bounds.top) * 64) / 64
      cellWidth = cells.width / term.cols; cellHeight = cells.height
      // Effects are deliberately lower resolution than xterm; original text stays sharp.
      for (const canvas of [current, history, reflectionSource, attenuation]) {
        canvas.width = Math.ceil(width * scale); canvas.height = Math.ceil(height * scale)
      }
      opticsDirty = true
      reflected.width = Math.ceil(bezel.clientWidth * .5); reflected.height = Math.ceil(bezel.clientHeight * .5)
      for (const canvas of [reflectionStill, reflectionHistory, reflectedAttenuation]) {
        canvas.width = reflected.width; canvas.height = reflected.height
      }
      reflectionCached = false
      ink.setTransform(scale, 0, 0, scale, 0, 0)
      ink.textBaseline = 'alphabetic'
      rowKeys = []; expires = 0; dirtyStart = 0; dirtyEnd = Infinity
      cursor = undefined; cursorKey = ''; nextBlink = Infinity
      measuring = false
      return true
    }

    const capture = (now: number, relight: boolean) => {
      const buffer = term.buffer.active, cell = buffer.getNullCell()
      const first = dirtyStart, last = Math.min(term.rows - 1, dirtyEnd)
      dirtyStart = Infinity; dirtyEnd = -1
      let changed = false
      for (let row = first; row <= last; row++) {
        const line = buffer.getLine(buffer.viewportY + row)
        const runs: { text: string; col: number; cells: number; fg: string; bg: string; font: string; dim: boolean; underline: boolean }[] = []
        let key = ''
        for (let col = 0; col < term.cols; col++) {
          if (!line?.getCell(col, cell) || !cell.getWidth()) continue
          const text = cell.isInvisible() ? '' : cell.getChars()
          const inverse = !!cell.isInverse()
          const fg = fgColor(cell, inverse)
          const bg = inverse ? fgColor(cell, false) : cell.isBgDefault() ? '' : fgColor(cell, true)
          if ((!text || text === ' ') && !bg) continue
          const font = `${cell.isItalic() ? 'italic' : 'normal'} ${cell.isBold() ? 'bold' : 'normal'} ${fontSize}px ${term.options.fontFamily}`
          const dim = !!cell.isDim(), underline = !!cell.isUnderline(), cells = cell.getWidth()
          key += `${col}:${text}:${cells}:${fg}:${bg}:${font}:${dim}:${underline};`
          const previous = runs[runs.length - 1]
          if (previous && previous.col + previous.cells === col && previous.fg === fg && previous.bg === bg
            && previous.font === font && previous.dim === dim && previous.underline === underline) {
            previous.text += text || ' '; previous.cells += cells
          } else runs.push({ text: text || ' ', col, cells, fg, bg, font, dim, underline })
        }
        if (!relight && rowKeys[row] === key) continue
        const y = top + row * cellHeight
        // Align row ownership to backing pixels so clearing a glowing row cannot
        // leave fractional-alpha slivers at its top/bottom boundary.
        const py = Math.max(0, Math.round(y * scale))
        const ph = Math.min(history.height - py, Math.round((y + cellHeight) * scale) - py)
        const retain = !relight && !!rowKeys[row] && afterglow > 0 && !reducedMotion.matches
        if (retain && ph > 0) {
          ghost.drawImage(current, 0, py, current.width, ph, 0, py, current.width, ph)
          expires = now + fadeTime
        }
        ink.save()
        ink.resetTransform()
        ink.clearRect(0, py, current.width, ph)
        ink.beginPath(); ink.rect(0, py, current.width, ph); ink.clip()
        ink.setTransform(scale, 0, 0, scale, 0, 0)
        for (const run of runs) {
          const x = left + run.col * cellWidth, runWidth = run.cells * cellWidth
          ink.globalAlpha = 1
          if (run.bg) { ink.fillStyle = run.bg; ink.fillRect(x, y, runWidth, cellHeight) }
          ink.globalAlpha = run.dim ? .5 : 1
          ink.fillStyle = run.fg; ink.font = run.font
          const baseline = y + (cellHeight - fontSize) / 2 + fontSize * .8
          // Each shadow is emitted offscreen so translucent halos do not repaint
          // the glyph. Share radii/colors with the visible DOM text.
          for (const halo of terminalLightHalos(optics.current.glow, optics.current.bloom, TERMINAL_PROFILES[profile].rgb)) {
            ink.save()
            ink.shadowColor = halo.color === 'currentColor' ? run.fg : halo.color
            ink.shadowBlur = halo.radius * scale
            ink.shadowOffsetX = width * scale * 2
            ink.fillText(run.text, x - width * 2, baseline, runWidth)
            ink.restore()
          }
          ink.fillText(run.text, x, baseline, runWidth)
          if (run.underline) ink.fillRect(x, y + cellHeight - 2, runWidth, 1)
        }
        ink.restore()
        ink.globalAlpha = 1
        if (expires && ph > 0) {
          // Mask live glyphs out of history. Only erased/moved text should linger.
          ghost.globalCompositeOperation = 'destination-out'
          ghost.drawImage(current, 0, py, current.width, ph, 0, py, current.width, ph)
          ghost.globalCompositeOperation = 'source-over'
        }
        rowKeys[row] = key; changed = true
      }
      return changed
    }

    const paintCursor = (context: CanvasRenderingContext2D, value: CursorImage) => {
      context.save()
      context.setTransform(scale, 0, 0, scale, 0, 0)
      context.fillStyle = context.strokeStyle = theme.cursor ?? theme.foreground ?? '#ffbc69'
      if (value.outline) {
        context.lineWidth = 1
        context.strokeRect(value.x + .5, value.y + .5, value.w - 1, value.h - 1)
      } else context.fillRect(value.x, value.y, value.w, value.h)
      context.restore()
    }

    const captureCursor = (now: number) => {
      const element = surface.querySelector<HTMLElement>('.xterm-cursor')
      let next: CursorImage | undefined
      nextBlink = Infinity
      if (element) {
        const bounds = content.getBoundingClientRect(), rect = element.getBoundingClientRect()
        let lit = true
        // xterm's DOM renderer blinks through CSS, without emitting onRender.
        // Wake at its actual half-cycle boundary instead of polling every frame.
        const animation = element.getAnimations().find(item => item.playState === 'running')
        if (animation) {
          const duration = Number(animation.effect?.getComputedTiming().duration)
          if (duration > 0 && typeof animation.currentTime === 'number') {
            const phase = (animation.currentTime % duration) / duration
            lit = phase < .5
            nextBlink = now + ((phase < .5 ? .5 : 1) - phase) * duration + 12
          }
        }
        // DOMRects include animated transforms in float32 precision. Normalize
        // to layout units so subpixel jitter cannot keep refreshing cursor trails.
        const layoutUnit = (value: number) => Math.round(value * 64) / 64
        next = { x: layoutUnit(rect.left - bounds.left), y: layoutUnit(rect.top - bounds.top),
          w: layoutUnit(rect.width), h: layoutUnit(rect.height), outline: element.classList.contains('xterm-cursor-outline'), lit }
        if (element.classList.contains('xterm-cursor-bar')) next.w = term.options.cursorWidth ?? 1
        if (element.classList.contains('xterm-cursor-underline')) { next.y += next.h - 1; next.h = 1 }
      }
      const key = next ? `${next.x}:${next.y}:${next.w}:${next.h}:${next.outline}:${next.lit}` : ''
      if (key === cursorKey) return false
      if (cursor?.lit && afterglow > 0 && !reducedMotion.matches) {
        paintCursor(ghost, cursor)
        expires = now + fadeTime
      }
      cursor = next; cursorKey = key
      if (cursor?.lit && !cursor.outline) {
        ghost.clearRect(cursor.x * scale, cursor.y * scale, cursor.w * scale, cursor.h * scale)
      }
      return true
    }

    // Reuse the exact reflected image when a settled source returns to one of
    // the six discrete jitter positions. No re-sampling or lower frame rate.
    const settledReflections = new Map<string, HTMLCanvasElement>()
    let settledVersion = -1
    const reflectLight = (target: CanvasRenderingContext2D, source: HTMLCanvasElement) => {
      const signal = getTerminalSignal(surface)
      const cacheable = source === reflectionSource && !expires && !signal?.offsets.some(value => value)
      if (settledVersion !== sourceVersion) {
        settledReflections.clear(); settledVersion = sourceVersion
      }
      const key = `${signal?.rgbShift ?? 0}:${signal?.jitterX ?? 0}:${signal?.jitterY ?? 0}`
      const cached = cacheable ? settledReflections.get(key) : undefined
      if (cached) {
        target.clearRect(0, 0, target.canvas.width, target.canvas.height)
        target.drawImage(cached, 0, 0)
        return
      }
      reflectFrame(target, paintSignal(source, surface, sourceVersion), curvature, reflected.width / bezel.clientWidth)
      if (cacheable) {
        // Jitter has five distinct positions; bound storage for future variants.
        if (settledReflections.size >= 6) settledReflections.delete(settledReflections.keys().next().value!)
        const snapshot = document.createElement('canvas')
        snapshot.width = target.canvas.width; snapshot.height = target.canvas.height
        snapshot.getContext('2d')!.drawImage(target.canvas, 0, 0)
        settledReflections.set(key, snapshot)
      }
    }
    const drawLiveLight = () => {
      light.drawImage(current, 0, 0)
      if (cursor?.lit) paintCursor(light, cursor)
    }
    const renderReflection = (changed: boolean, decay: number) => {
      if (!reflection || !curvature) return
      // During fading, reuse the mirrored frames and just attenuate the old light.
      // Continuous output keeps the single-pass path; blinking does not resample
      // hundreds of bezel slices on every decay tick.
      if (!changed) {
        if (!reflectionCached) {
          light.clearRect(0, 0, reflectionSource.width, reflectionSource.height)
          drawLiveLight()
          reflectLight(still, reflectionSource)
          reflectLight(echo, history)
          reflectionCached = true
        } else if (expires) {
          echo.globalCompositeOperation = 'destination-in'
          echo.fillStyle = `rgba(0,0,0,${decay})`
          echo.fillRect(0, 0, reflectionHistory.width, reflectionHistory.height)
          echo.globalCompositeOperation = 'source-over'
        }
        if (!expires) echo.clearRect(0, 0, reflectionHistory.width, reflectionHistory.height)
        frame.clearRect(0, 0, reflected.width, reflected.height)
        frame.globalAlpha = afterglowOpacity(afterglow); frame.drawImage(reflectionHistory, 0, 0)
        frame.globalAlpha = 1; frame.drawImage(reflectionStill, 0, 0)
        return
      }
      reflectionCached = false
      light.clearRect(0, 0, reflectionSource.width, reflectionSource.height)
      light.globalAlpha = afterglowOpacity(afterglow)
      if (expires) light.drawImage(history, 0, 0)
      light.globalAlpha = 1
      drawLiveLight()
      reflectLight(frame, reflectionSource)
    }

    const tick = () => {
      timer = undefined
      if (!visible()) return
      const now = performance.now()
      lastTick = now
      if (measuring && !measure()) return
      const opticsChanged = opticsDirty
      if (opticsDirty) {
        paintScreenAttenuation(attenuation, width, height, optics.current)
        reflectFrame(reflectedMask, attenuation, curvature, reflected.width / bezel.clientWidth)
        // A static compositor mask shades live light and cached trails together.
        // Rebuild only for optics/size changes, avoiding per-frame canvas readbacks.
        reflectedMask.globalCompositeOperation = 'source-out'
        reflectedMask.fillStyle = '#fff'
        reflectedMask.fillRect(0, 0, reflectedAttenuation.width, reflectedAttenuation.height)
        reflectedMask.globalCompositeOperation = 'source-over'
        const mask = `url("${reflectedAttenuation.toDataURL()}")`
        reflected.style.maskImage = mask
        reflected.style.webkitMaskImage = mask
        opticsDirty = false
      }
      const faded = expires > 0
      const decay = Math.exp(-(now - lastDecay) / (fadeTime / 4))
      if (expires && now >= expires) clearHistory()
      else if (expires) {
        ghost.globalCompositeOperation = 'destination-in'
        ghost.fillStyle = `rgba(0,0,0,${decay})`
        ghost.fillRect(0, 0, history.width, history.height)
        ghost.globalCompositeOperation = 'source-over'
      }
      lastDecay = now
      const haloKey = `${optics.current.glow}:${optics.current.bloom}`
      const relight = capturedHalos !== haloKey
      if (relight) { dirtyStart = 0; dirtyEnd = Infinity }
      const changed = dirtyStart !== Infinity && (afterglow > 0 || reflection > 0) ? capture(now, relight) : false
      capturedHalos = haloKey
      const cursorChanged = captureCursor(now)
      if (changed || faded || cursorChanged || opticsChanged) sourceVersion++
      if (changed || faded || cursorChanged || opticsChanged || signalDirty) renderReflection(changed || cursorChanged || opticsChanged || signalDirty, decay)
      reflected.dataset.signalFrame = String(getTerminalSignal(surface)?.frame ?? 0)
      signalDirty = false
      if (expires) schedule()
      else if (Number.isFinite(nextBlink)) schedule(nextBlink - now)
    }
    const schedule = (delay = 0) => {
      if (!visible() || (!afterglow && !reflection)) return
      // New output may arrive before a pending cursor blink boundary.
      if (timer !== undefined) clearTimeout(timer)
      // Decay/maintenance ticks run at most 20 Hz; render/signal events flush synchronously.
      timer = setTimeout(tick, Math.max(delay, 0, 50 - (performance.now() - lastTick)))
    }
    const invalidate = (start = 0, end = Infinity) => {
      dirtyStart = Math.min(dirtyStart, start); dirtyEnd = Math.max(dirtyEnd, end)
      if (afterglow > 0 || reflection > 0) schedule()
    }
    refreshOptics.current = () => { opticsDirty = true; reflectionCached = false; schedule() }
    const visibility = () => {
      bezel.dataset.effectsPaused = visible() ? 'false' : 'true'
      if (timer !== undefined) { clearTimeout(timer); timer = undefined }
      // Hidden pages resume from a fresh screen, without replaying stale output.
      clearHistory(); rowKeys = []; measuring = true
      if (visible()) invalidate()
    }
    const flush = () => {
      if (!visible() || (!afterglow && !reflection)) return
      if (timer !== undefined) clearTimeout(timer)
      tick()
    }
    const signalChanged = () => { signalDirty = true; reflectionCached = false; flush() }
    const fontChanged = () => { measuring = true; invalidate() }
    surface.addEventListener(SIGNAL_EVENT, signalChanged)
    surface.addEventListener(FONT_EVENT, fontChanged)
    const rendered = term.onRender(({ start, end }) => { invalidate(start, end); flush() })
    const scrolled = term.onScroll(() => invalidate())
    const resized = new ResizeObserver(() => { measuring = true; invalidate() })
    resized.observe(surface)
    const intersection = new IntersectionObserver(entries => {
      const next = entries[0]?.isIntersecting ?? true
      if (next !== intersecting) { intersecting = next; visibility() }
    })
    intersection.observe(surface)
    document.addEventListener('visibilitychange', visibility)
    reducedMotion.addEventListener('change', visibility)
    document.fonts.ready.then(() => { if (!disposed) { measuring = true; invalidate() } }).catch(() => {})
    visibility()
    return () => {
      disposed = true
      refreshOptics.current = undefined
      if (timer !== undefined) clearTimeout(timer)
      surface.removeEventListener(SIGNAL_EVENT, signalChanged)
      surface.removeEventListener(FONT_EVENT, fontChanged)
      rendered.dispose(); scrolled.dispose(); resized.disconnect(); intersection.disconnect()
      reflectFrame.dispose(); settledReflections.clear()
      document.removeEventListener('visibilitychange', visibility)
      reducedMotion.removeEventListener('change', visibility)
      clearHistory(); frame.clearRect(0, 0, reflected.width, reflected.height)
      reflected.style.maskImage = ''; reflected.style.webkitMaskImage = ''
      delete bezel.dataset.effectsPaused; delete reflected.dataset.signalFrame
    }
  }, [id, surfaceRef, historyRef, reflectionRef, enabled, profile, afterglow, reflection, curvature, fontSize, appearance.font])
  useEffect(() => {
    optics.current = { vignette: appearance.vignette, scanlines: appearance.scanlines, glow: appearance.glow, bloom: appearance.bloom, pixelGrid: appearance.pixelGrid, pixelSize: appearance.pixelSize }
    refreshOptics.current?.()
  }, [appearance.vignette, appearance.scanlines, appearance.glow, appearance.bloom, appearance.pixelGrid, appearance.pixelSize])
}
