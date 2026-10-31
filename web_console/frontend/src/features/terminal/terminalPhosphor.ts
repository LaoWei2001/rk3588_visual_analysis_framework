import { useEffect, useRef } from 'react'
import type { RefObject } from 'react'
import type { IBufferCell, ITheme } from '@xterm/xterm'
import { getSession } from './terminalSession'
import { TERMINAL_PROFILES, terminalAppearanceOptions } from './terminalAppearance'
import type { TerminalAppearance } from './terminalAppearance'
import { TERMINAL_REFLECTION_MATERIALS } from './terminalReflectionMaterial'
import type { TerminalReflectionMaterial } from './terminalReflectionMaterial'
import { buildReflectionTransport } from './terminalReflectionTransport'
import { BEZEL_JOINT_WIDTH, SCREEN_RECESS, bezelJoints } from './terminalBezelGeometry'
import { createSignalPainter, getTerminalSignal, SIGNAL_EVENT, FONT_EVENT } from './terminalSignal'
import { createReflectionRaster } from './terminalReflectionRaster'
import { afterglowOpacity, terminalLightHalos, paintScreenAttenuation, terminalDotGain } from './terminalOptics'
import { EFFECTS_VISIBILITY_EVENT, effectsPaused } from './terminalVisibility'
import { createTerminalContrast } from './terminalContrast'

// Rendering ideas: cool-retro-term's terminal_dynamic/static.frag and BurnInEffect.qml.
// Independent Canvas implementation: cached noise, single-bounce liner illumination, decaying history.
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

/** Geometry is independent of the terminal output and is traced only on resize. */
function createBezelReflector() {
  let raster: ReturnType<typeof createReflectionRaster> | undefined
  let key = ''
  let joints: ReturnType<typeof bezelJoints> = []
  const reflect = (target: CanvasRenderingContext2D, source: HTMLCanvasElement, curvature: number, pixelScale: number, material: TerminalReflectionMaterial, inset: number) => {
    const w = target.canvas.width, h = target.canvas.height
    target.clearRect(0, 0, w, h)
    // Route changes can detach a split pane before its resize notification.
    // Never build a transport for a zero-sized or non-finite projected opening.
    if (!curvature || w <= 0 || h <= 0 || !Number.isFinite(pixelScale) || pixelScale <= 0
      || w / pixelScale <= inset * 2 || h / pixelScale <= inset * 2) return
    raster ??= createReflectionRaster()
    const nextKey = `${w}:${h}:${curvature}:${pixelScale}:${material}:${inset}`
    if (key !== nextKey) {
      raster.configure(buildReflectionTransport(w / pixelScale, h / pixelScale, curvature, material, inset))
      joints = bezelJoints(w, h, curvature / 500, inset * pixelScale)
      key = nextKey
    }
    target.drawImage(raster.draw(source), 0, 0, w, h)
  }
  reflect.composite = (target: CanvasRenderingContext2D, strength: number) => {
    if(!raster)return
    const result=raster.composite(target.canvas,strength)
    target.clearRect(0,0,target.canvas.width,target.canvas.height)
    target.drawImage(result,0,0,target.canvas.width,target.canvas.height)
    // Cut joints after composition so reconstruction cannot light their gaps.
    target.save(); target.globalCompositeOperation = 'destination-out'
    target.lineWidth = BEZEL_JOINT_WIDTH * target.canvas.width / (target.canvas.clientWidth || target.canvas.width)
    target.beginPath()
    for (const [outer, inner] of joints) { target.moveTo(...outer); target.lineTo(...inner) }
    target.stroke(); target.restore()
  }
  reflect.dispose = () => raster?.dispose()
  return reflect
}

export function useTerminalPhosphor(id: string, surfaceRef: RefObject<HTMLDivElement>,
  historyRef: RefObject<HTMLCanvasElement>, reflectionRef: RefObject<HTMLCanvasElement>,
  appearance: TerminalAppearance, fontSize: number, inset = SCREEN_RECESS) {
  const { enabled, profile, afterglow, reflection, reflectionMaterial, curvature } = appearance
  const optics = useRef({ brightness: appearance.brightness, vignette: appearance.vignette, curvature: appearance.curvature, scanlines: appearance.scanlines, glow: appearance.glow, bloom: appearance.bloom, pixelGrid: appearance.pixelGrid, pixelSize: appearance.pixelSize })
  const refreshOptics = useRef<() => void>()
  useEffect(() => {
    const term = getSession(id)?.term
    const surface = surfaceRef.current, history = historyRef.current, reflected = reflectionRef.current
    if (!term || !surface || !history || !reflected) return
    const ghost = history.getContext('2d'), frame = reflected.getContext('2d')
    if (!ghost || !frame) return
    ghost.clearRect(0, 0, history.width, history.height)
    frame.clearRect(0, 0, reflected.width, reflected.height)
    const hasReflection = reflection > 0 && curvature > 0
    if (!enabled || (!afterglow && !hasReflection)) return

    const current = document.createElement('canvas'), reflectionSource = document.createElement('canvas')
    const ink = current.getContext('2d')!, light = reflectionSource.getContext('2d')!
    const attenuation = document.createElement('canvas')
    const incidentLight = document.createElement('canvas')
    const incident = incidentLight.getContext('2d')!
    let opticsDirty = true, capturedHalos = ''
    let signalDirty = true, emissionDirty = false, sourceVersion = 0
    const paintSignal = createSignalPainter()
    const reflectFrame = createBezelReflector()
    const theme = terminalAppearanceOptions(appearance).theme!
    const indexedColor = palette(theme)
    const readableColor = createTerminalContrast()
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
    const decayHistory = (now: number) => {
      const decay = Math.exp(-Math.max(0, now - lastDecay) / (fadeTime / 4))
      if (expires && now >= expires) clearHistory()
      else if (expires) {
        ghost.globalCompositeOperation = 'destination-in'
        ghost.fillStyle = `rgba(0,0,0,${decay})`
        ghost.fillRect(0, 0, history.width, history.height)
        ghost.globalCompositeOperation = 'source-over'
      }
      lastDecay = now
      return decay
    }
    const fgColor = (cell: IBufferCell, background: boolean, bold = false) => {
      const rgb = background ? cell.isBgRGB() : cell.isFgRGB()
      const indexed = background ? cell.isBgPalette() : cell.isFgPalette()
      const color = background ? cell.getBgColor() : cell.getFgColor()
      if (rgb) return `#${color.toString(16).padStart(6, '0')}`
      if (indexed) return indexedColor(color + (bold && color < 8 ? 8 : 0))
      return (background ? theme.background : theme.foreground) ?? '#ffbc69'
    }

    const measure = () => {
      const row = surface.querySelector('.xterm-rows > div')
      if (!row) return false
      const bounds = content.getBoundingClientRect(), cells = row.getBoundingClientRect()
      width = surface.clientWidth; height = surface.clientHeight
      if (!width || !height || !cells.width || !cells.height || !bezel.clientWidth || !bezel.clientHeight) return false
      scale = Math.min(1, 1100 / width)
      left = Math.round((cells.left - bounds.left) * 64) / 64; top = Math.round((cells.top - bounds.top) * 64) / 64
      cellWidth = cells.width / term.cols; cellHeight = cells.height
      // Effects are deliberately lower resolution than xterm; original text stays sharp.
      for (const canvas of [current, history, reflectionSource, attenuation, incidentLight]) {
        canvas.width = Math.ceil(width * scale); canvas.height = Math.ceil(height * scale)
      }
      opticsDirty = true
      // Keep the sharp material at its transport resolution through composition;
      // half-size intermediate canvases would blur it twice during resampling.
      const reflectionScale = Math.min(1, TERMINAL_REFLECTION_MATERIALS[reflectionMaterial].maxEdge
        / Math.max(bezel.clientWidth, bezel.clientHeight))
      reflected.width = Math.ceil(bezel.clientWidth * reflectionScale)
      reflected.height = Math.ceil(bezel.clientHeight * reflectionScale)
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
      // Use the DOM renderer's committed rectangles: these already account for
      // scrollback, rectangular selection and the focused/inactive palette.
      // Inline layout coordinates avoid applying the CRT warp a second time.
      const selections = Array.from(surface.querySelectorAll<HTMLElement>('.xterm-selection > div'), element => ({
        startRow: Math.round(parseFloat(element.style.top) / cellHeight),
        endRow: Math.round((parseFloat(element.style.top) + parseFloat(element.style.height)) / cellHeight),
        startCol: Math.round(parseFloat(element.style.left) / cellWidth),
        endCol: Math.round((parseFloat(element.style.left) + parseFloat(element.style.width)) / cellWidth),
        background: getComputedStyle(element).backgroundColor,
      }))
      let changed = false
      const domRows = surface.querySelector('.xterm-rows')?.children
      for (let row = first; row <= last; row++) {
        const line = buffer.getLine(buffer.viewportY + row)
        const selection = selections.find(s => row >= s.startRow && row < s.endRow)
        // The DOM renderer may choose a different contrast color for inverse,
        // selected or dim cells. Sample its final foreground on dirty rows only;
        // re-deriving it from ANSI attributes can turn a visible white glyph black.
        const domRow = domRows?.item(row), rowLeft = domRow?.getBoundingClientRect().left ?? 0
        const colors = Array.from(domRow?.children ?? [], element => {
          const rect = element.getBoundingClientRect()
          return { start: Math.round((rect.left - rowLeft) / cellWidth),
            end: Math.round((rect.right - rowLeft) / cellWidth),
            color: element.classList.contains('xterm-cursor') ? undefined : getComputedStyle(element).color }
        })
        let colorIndex = 0
        const runs: { text: string; col: number; cells: number; fg: string; bg: string; font: string; dim: boolean; underline: boolean }[] = []
        let key = ''
        for (let col = 0; col < term.cols; col++) {
          if (!line?.getCell(col, cell) || !cell.getWidth()) continue
          const text = cell.isInvisible() ? '' : cell.getChars()
          const inverse = !!cell.isInverse()
          const rawFg = fgColor(cell, inverse, !!cell.isBold())
          const selected = selection && col >= selection.startCol && col < selection.endCol
          const bg = selected ? selection.background : inverse ? fgColor(cell, false) : cell.isBgDefault() ? '' : fgColor(cell, true)
          while (colorIndex < colors.length && colors[colorIndex].end <= col) colorIndex++
          const displayed = colors[colorIndex]
          const resolvedColor = displayed && displayed.start <= col ? displayed.color : undefined
          const fg = resolvedColor ?? readableColor(rawFg, bg || theme.background!, !!cell.isDim(), cell.getCode())
          if ((!text || text === ' ') && !bg) continue
          const font = `${cell.isItalic() ? 'italic' : 'normal'} ${cell.isBold() ? 'bold' : 'normal'} ${fontSize}px ${term.options.fontFamily}`
          // Computed DOM colors already include dim opacity; do not dim twice.
          const dim = !resolvedColor && !!cell.isDim(), underline = !!cell.isUnderline(), cells = cell.getWidth()
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
        const retain = !relight && !!rowKeys[row] && afterglow > 0 && !reducedMotion.matches && !effectsPaused(surface)
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
          for (const halo of run.bg ? [] : terminalLightHalos(optics.current.glow, optics.current.bloom, TERMINAL_PROFILES[profile].rgb)) {
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
      if (cursor?.lit && afterglow > 0 && !reducedMotion.matches && !effectsPaused(surface)) {
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
      // Apply optical loss BEFORE integration: many screen points illuminate
      // each liner point, so an independently reflected alpha mask is invalid.
      incident.clearRect(0, 0, incidentLight.width, incidentLight.height)
      const gain = optics.current.brightness / 100 * terminalDotGain(optics.current.pixelSize, optics.current.pixelGrid)
      incident.filter = `brightness(${gain})`
      incident.drawImage(paintSignal(source, surface, sourceVersion), 0, 0)
      incident.filter = 'none'
      incident.globalCompositeOperation = 'destination-out'
      incident.drawImage(attenuation, 0, 0)
      incident.globalCompositeOperation = 'source-over'
      reflectFrame(target, incidentLight, curvature, reflected.width / bezel.clientWidth, reflectionMaterial, inset)
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
    const renderReflection = () => {
      if (!reflection || !curvature) return
      // Integrate the same composed emission on every fading frame. Scaling an
      // already sRGB-encoded reflection gives a different decay than scaling the
      // source, causing brightness jumps whenever a signal update bypasses it.
      light.clearRect(0, 0, reflectionSource.width, reflectionSource.height)
      light.globalAlpha = afterglowOpacity(afterglow)
      if (expires) light.drawImage(history, 0, 0)
      light.globalAlpha = 1
      drawLiveLight()
      reflectLight(frame, reflectionSource)
      reflectFrame.composite(frame, reflection / 100)
    }

    const tick = () => {
      timer = undefined
      if (!visible()) return
      const now = performance.now()
      lastTick = now
      if (measuring && !measure()) return
      const opticsChanged = opticsDirty
      if (opticsDirty && hasReflection) {
        paintScreenAttenuation(attenuation, width, height, optics.current)
      }
      opticsDirty = false
      const faded = expires > 0
      decayHistory(now)
      const haloKey = `${optics.current.glow}:${optics.current.bloom}`
      const relight = capturedHalos !== haloKey
      if (relight) { dirtyStart = 0; dirtyEnd = Infinity }
      const captured = dirtyStart !== Infinity && (afterglow > 0 || reflection > 0) ? capture(now, relight) : false
      const changed = captured || emissionDirty
      emissionDirty = false
      capturedHalos = haloKey
      const cursorChanged = captureCursor(now)
      if (changed || faded || cursorChanged || opticsChanged) sourceVersion++
      if (changed || faded || cursorChanged || opticsChanged || signalDirty) renderReflection()
      reflected.dataset.signalFrame = String(getTerminalSignal(surface)?.frame ?? 0)
      signalDirty = false
      if (expires) schedule()
      else if (!effectsPaused(surface) && Number.isFinite(nextBlink)) schedule(nextBlink - now)
    }
    const schedule = (delay = 0) => {
      if (!visible() || (!afterglow && !reflection)) return
      // New output may arrive before a pending cursor blink boundary.
      if (timer !== undefined) clearTimeout(timer)
      // Coalesce terminal output and decay at 20 Hz. Signal changes still commit
      // synchronously so text displacement and its reflected light stay aligned.
      timer = setTimeout(tick, Math.max(delay, 0, 50 - (performance.now() - lastTick)))
    }
    const invalidate = (start = 0, end = Infinity) => {
      dirtyStart = Math.min(dirtyStart, start); dirtyEnd = Math.max(dirtyEnd, end)
      if (afterglow > 0 || reflection > 0) schedule()
    }
    refreshOptics.current = () => { opticsDirty = true; schedule() }
    const visibility = () => {
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
    const signalChanged = () => { signalDirty = true; flush() }
    const visibilityChanged = () => {
      if (timer !== undefined) { clearTimeout(timer); timer = undefined }
      if (effectsPaused(surface)) clearHistory()
      // Commit the steady cursor and settled reflection once, then stop timers.
      signalDirty = true; flush()
    }
    const fontChanged = () => { measuring = true; invalidate() }
    surface.addEventListener(SIGNAL_EVENT, signalChanged)
    surface.addEventListener(FONT_EVENT, fontChanged)
    surface.addEventListener(EFFECTS_VISIBILITY_EVENT, visibilityChanged)
    const rendered = term.onRender(({ start, end }) => {
      invalidate(start, end)
      if (!afterglow || measuring || !visible()) return
      // xterm has just changed the DOM, but the browser has not painted it yet.
      // Hand the old rows to persistence now, avoiding a blank frame followed
      // by a bright reappearance. Reflection work stays coalesced at 20 Hz.
      const now = performance.now(), haloKey = `${optics.current.glow}:${optics.current.bloom}`
      if (expires) emissionDirty = true
      decayHistory(now)
      if (capture(now, capturedHalos !== haloKey)) {
        emissionDirty = true
      }
      capturedHalos = haloKey
    })
    const scrolled = term.onScroll(() => invalidate())
    // Selection changes need no terminal output. Observe the committed DOM as
    // well as the public event, since xterm can paint selection on a later frame.
    const selected = term.onSelectionChange(() => invalidate())
    const selectionChanged = () => invalidate()
    const selectionObserver = new MutationObserver(selectionChanged)
    const selectionContainer = surface.querySelector('.xterm-selection')
    if (selectionContainer) selectionObserver.observe(selectionContainer, { childList: true, subtree: true, attributes: true })
    surface.addEventListener('focusin', selectionChanged)
    surface.addEventListener('focusout', selectionChanged)
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
      surface.removeEventListener(EFFECTS_VISIBILITY_EVENT, visibilityChanged)
      rendered.dispose(); scrolled.dispose(); resized.disconnect(); intersection.disconnect()
      selected.dispose(); selectionObserver.disconnect()
      surface.removeEventListener('focusin', selectionChanged)
      surface.removeEventListener('focusout', selectionChanged)
      reflectFrame.dispose(); settledReflections.clear()
      document.removeEventListener('visibilitychange', visibility)
      reducedMotion.removeEventListener('change', visibility)
      clearHistory(); frame.clearRect(0, 0, reflected.width, reflected.height)
      reflected.style.maskImage = ''; reflected.style.webkitMaskImage = ''
      delete reflected.dataset.signalFrame
    }
  }, [id, surfaceRef, historyRef, reflectionRef, enabled, profile, afterglow, reflection, reflectionMaterial, curvature, inset, fontSize, appearance.font])
  useEffect(() => {
    optics.current = { brightness: appearance.brightness, vignette: appearance.vignette, curvature: appearance.curvature, scanlines: appearance.scanlines, glow: appearance.glow, bloom: appearance.bloom, pixelGrid: appearance.pixelGrid, pixelSize: appearance.pixelSize }
    refreshOptics.current?.()
  }, [appearance.brightness, appearance.vignette, appearance.curvature, appearance.scanlines, appearance.glow, appearance.bloom, appearance.pixelGrid, appearance.pixelSize])
}
