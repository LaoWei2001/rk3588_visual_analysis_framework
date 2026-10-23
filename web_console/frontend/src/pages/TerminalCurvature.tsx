import { useEffect, useMemo, useState } from 'react'
import type { RefObject } from 'react'
import TerminalBezel from './TerminalBezel'
import { SCREEN_RECESS } from './terminalBezelGeometry'
import { signalOffset, getTerminalSignal } from './terminalSignal'

const MAP_SIZE = 256
const MAX_CURVATURE = 0.2
const mappedEvents = new WeakSet<MouseEvent>()
let draggingSurface: HTMLDivElement | null = null

function displacementMap(width: number, height: number): string {
  const canvas = document.createElement('canvas')
  canvas.width = canvas.height = MAP_SIZE
  const context = canvas.getContext('2d')
  if (!context) return ''
  const pixels = context.createImageData(MAP_SIZE, MAP_SIZE)
  const longest = Math.max(width, height)
  for (let row = 0; row < MAP_SIZE; row++) {
    const y = 2 * (row + 0.5) / MAP_SIZE - 1
    for (let col = 0; col < MAP_SIZE; col++) {
      const x = 2 * (col + 0.5) / MAP_SIZE - 1
      const offset = (row * MAP_SIZE + col) * 4
      pixels.data[offset] = Math.round(128 + 127 * x * y * y * width / longest)
      pixels.data[offset + 1] = Math.round(128 + 127 * y * x * x * height / longest)
      pixels.data[offset + 2] = 128
      pixels.data[offset + 3] = 255
    }
  }
  context.putImageData(pixels, 0, 0)
  return canvas.toDataURL()
}

/** Curve only the terminal image, keeping xterm selection and mouse reports aligned. */
export default function TerminalCurvature({ filterId, surfaceRef, strength, horizontalSync }: {
  filterId: string; surfaceRef: RefObject<HTMLDivElement>; strength: number; horizontalSync: number
}) {
  const [size, setSize] = useState({ width: 0, height: 0 })
  const amount = Math.min(100, Math.max(0, strength)) / 100 * MAX_CURVATURE
  const enabled = amount > 0
  const map = useMemo(() => size.width && size.height
    ? displacementMap(size.width, size.height) : '', [size.width, size.height])

  useEffect(() => {
    const surface = surfaceRef.current
    if (!surface || !enabled) return
    const resize = () => {
      const width = surface.clientWidth, height = surface.clientHeight
      setSize(previous => previous.width === width && previous.height === height
        ? previous : { width, height })
    }
    resize()
    const observer = new ResizeObserver(resize)
    observer.observe(surface)
    return () => observer.disconnect()
  }, [surfaceRef, enabled])

  useEffect(() => {
    const surface = surfaceRef.current
    if (!surface || !amount || !map) return
    surface.style.filter = `url(#${filterId})`
    return () => { surface.style.filter = '' }
  }, [surfaceRef, amount, filterId, map])

  useEffect(() => {
    const surface = surfaceRef.current
    if (!surface || (!amount && !horizontalSync) || (amount && !map)) return
    const mapMouse = (event: MouseEvent) => {
      if (mappedEvents.has(event)) return
      const inside = event.target instanceof Node && surface.contains(event.target)
      if (event.type === 'mousedown' && inside && event.button === 0) draggingSurface = surface
      if (draggingSurface && draggingSurface !== surface) return
      const shouldMap = inside || draggingSurface === surface
      if (event.type === 'mouseup') draggingSurface = null
      if (!shouldMap) return
      const rect = surface.getBoundingClientRect()
      if (!rect.width || !rect.height) return
      mappedEvents.add(event)
      // SVG samples SourceGraphic here. Apply the same mapping before xterm's
      // own document listeners handle selection or generate SGR mouse reports.
      const x = (event.clientX - rect.left) / rect.width * 2 - 1
      const y = (event.clientY - rect.top) / rect.height * 2 - 1
      const clientY = rect.top + (1 + y * (1 + amount * x * x)) * rect.height / 2
      const clientX = rect.left + (1 + x * (1 + amount * y * y)) * rect.width / 2
        + signalOffset(surface, clientY - rect.top - (getTerminalSignal(surface)?.jitterY ?? 0))
      Object.defineProperties(event, {
        clientX: { configurable: true, value: clientX },
        clientY: { configurable: true, value: clientY },
        pageX: { configurable: true, value: clientX + window.scrollX },
        pageY: { configurable: true, value: clientY + window.scrollY },
      })
    }
    const events = ['mousedown', 'mousemove', 'mouseup', 'click', 'dblclick', 'contextmenu', 'wheel'] as const
    for (const type of events) document.addEventListener(type, mapMouse, true)
    const stopDragging = () => { if (draggingSurface === surface) draggingSurface = null }
    window.addEventListener('blur', stopDragging)
    return () => {
      stopDragging()
      for (const type of events) document.removeEventListener(type, mapMouse, true)
      window.removeEventListener('blur', stopDragging)
    }
  }, [surfaceRef, amount, map, horizontalSync])

  if (!amount || !map) return null
  return (
    <>
    <TerminalBezel width={size.width + SCREEN_RECESS * 2} height={size.height + SCREEN_RECESS * 2} amount={amount} id={`${filterId}-bezel`} />
    <svg className="term-curvature-defs" width="0" height="0" aria-hidden="true" focusable="false">
      <defs>
        <filter id={filterId} filterUnits="userSpaceOnUse" primitiveUnits="userSpaceOnUse"
          x="0" y="0" width={size.width} height={size.height} colorInterpolationFilters="sRGB">
          <feImage href={map} x="0" y="0" width={size.width} height={size.height}
            preserveAspectRatio="none" result="coordinates" />
          <feComponentTransfer in="coordinates" result="warp">
            {/* Center byte 128 maps exactly to 0.5, keeping the middle sharp. */}
            <feFuncR type="linear" slope={255 / 254} intercept={-1 / 254} />
            <feFuncG type="linear" slope={255 / 254} intercept={-1 / 254} />
          </feComponentTransfer>
          <feDisplacementMap in="SourceGraphic" in2="warp" xChannelSelector="R" yChannelSelector="G"
            scale={Math.max(size.width, size.height) * amount} result="curvedScreen" />
          <feDropShadow in="curvedScreen" dx="0" dy="3" stdDeviation="4" floodColor="#000000" floodOpacity="0.8" />
          <feDropShadow dx="0" dy="-0.8" stdDeviation="0.6" floodColor="var(--crt-foreground)" floodOpacity="0.22" />
        </filter>
      </defs>
    </svg>
    </>
  )
}
