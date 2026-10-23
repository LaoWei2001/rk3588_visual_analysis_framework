import { useEffect, useRef } from 'react'
import type { RefObject } from 'react'
import { SIGNAL_ROWS, setTerminalSignal, signalMapImage } from './terminalSignal'

/** A small wave texture drives line-dependent horizontal sync errors. */
export default function TerminalSignal({ id, surfaceRef, horizontalSync, rgbShift, jitter }: {
  id: string; surfaceRef: RefObject<HTMLDivElement>; horizontalSync: number; rgbShift: number; jitter: number
}) {
  const mapRef = useRef<SVGFEImageElement>(null)
  const displacementRef = useRef<SVGFEDisplacementMapElement>(null)
  const shift = rgbShift / 100 * 2.5
  useEffect(() => {
    const surface = surfaceRef.current
    const content = surface?.querySelector<HTMLElement>('.term-phosphor-content')
    if (!surface || !content || (!horizontalSync && !rgbShift && !jitter)) return
    const pixels = new Uint8Array(SIGNAL_ROWS)
    const offsets = new Float32Array(SIGNAL_ROWS)
    const reduced = matchMedia('(prefers-reduced-motion: reduce)')
    let timer: ReturnType<typeof setTimeout> | undefined, visible = true, disposed = false, selecting = false
    let phase = Math.random() * 6, frequency = 2, burstStart = 0, duration = 0, nextBurst = 0, frame = 0
    const active = () => !disposed && !selecting && visible && !document.hidden && !reduced.matches && (horizontalSync > 0 || jitter > 0)
    const maxShift = horizontalSync / 100 * 8
    displacementRef.current?.setAttribute('scale', String(maxShift * 2))
    let mapReady = false, previousX = NaN, previousY = NaN
    const paint = (envelope: number, jitterX = 0, jitterY = 0) => {
      let mapChanged = !mapReady
      for (let y = 0; y < SIGNAL_ROWS; y++) {
        const wave = Math.sin(y / SIGNAL_ROWS * Math.PI * 2 * frequency + phase)
        const byte = Math.round(128 + 127 * wave * envelope)
        if (pixels[y] !== byte) mapChanged = true
        pixels[y] = byte
        offsets[y] = (byte - 128) / 127 * maxShift
      }
      if (mapChanged) {
        mapRef.current?.setAttribute('href', signalMapImage(pixels))
        mapReady = true
      }
      if (!mapChanged && jitterX === previousX && jitterY === previousY) return
      previousX = jitterX; previousY = jitterY
      // Publish the same frame to the reflection sampler synchronously. Both
      // surfaces are committed in this task, before the browser paints either.
      setTerminalSignal(surface, { offsets, rgbShift: shift, jitterX, jitterY, frame: ++frame, filterId: id })
      content.style.transform = jitterX || jitterY ? `translate(${jitterX}px, ${jitterY}px)` : ''
      content.dataset.signalFrame = String(frame)
    }
    const tick = () => {
      timer = undefined
      if (!active()) { paint(0); return }
      const now = performance.now()
      let envelope = 0
      if (horizontalSync && now >= nextBurst) {
        if (!duration) { burstStart = now; duration = 480 + Math.random() * 420; frequency = 1.5 + Math.random() * 4 }
        const age = (now - burstStart) / duration
        if (age >= 1) { duration = 0; nextBurst = now + 900 + Math.random() * 1900 }
        else { phase += .35; envelope = Math.sin(Math.PI * Math.max(.12, age)) }
      }
      const steps = [[0, 0], [-1, 0], [1, 0], [0, 0], [-.5, 1], [.5, -1]]
      const sample = steps[Math.floor(Math.random() * steps.length)]
      paint(envelope, sample[0] * jitter / 100 * .85, sample[1] * jitter / 100 * .35)
      timer = setTimeout(tick, !jitter && nextBurst > now ? nextBurst - now : 100)
    }

    const visibility = () => {
      if (timer !== undefined) clearTimeout(timer)
      duration = 0; nextBurst = 0; paint(0)
      if (active()) timer = setTimeout(tick, 100)
    }
    // Hold the current wave while dragging so selection endpoints cannot drift.
    const hold = (event: MouseEvent) => {
      if (event.button !== 0) return
      selecting = true
      if (timer !== undefined) clearTimeout(timer)
    }
    const release = () => { if (selecting) { selecting = false; visibility() } }
    surface.addEventListener('mousedown', hold)
    document.addEventListener('mouseup', release)
    window.addEventListener('blur', release)
    const resize = () => {
      mapRef.current?.setAttribute('width', String(surface.clientWidth))
      mapRef.current?.setAttribute('height', String(surface.clientHeight))
    }
    content.style.filter = horizontalSync || rgbShift ? `url(#${id})` : ''
    const observer = new ResizeObserver(resize); observer.observe(surface); resize()
    const intersection = new IntersectionObserver(entries => { visible = entries[0]?.isIntersecting ?? true; visibility() })
    intersection.observe(surface)
    document.addEventListener('visibilitychange', visibility)
    reduced.addEventListener('change', visibility)
    visibility()
    return () => {
      disposed = true
      if (timer !== undefined) clearTimeout(timer)
      surface.removeEventListener('mousedown', hold)
      document.removeEventListener('mouseup', release); window.removeEventListener('blur', release)
      observer.disconnect(); intersection.disconnect()
      document.removeEventListener('visibilitychange', visibility); reduced.removeEventListener('change', visibility)
      content.style.filter = ''; content.style.transform = ''; delete content.dataset.signalFrame; setTerminalSignal(surface)
    }
  }, [id, surfaceRef, horizontalSync, rgbShift, shift, jitter])
  if (!horizontalSync && !rgbShift) return null
  return <svg className="term-signal-defs" aria-hidden="true" width="0" height="0">
    <defs>
      <filter id={`${id}-red-channel`} colorInterpolationFilters="sRGB">
        <feColorMatrix values="1 0 0 0 0  0 0 0 0 0  0 0 0 0 0  0 0 0 1 0" />
      </filter>
      <filter id={`${id}-green-channel`} colorInterpolationFilters="sRGB">
        <feColorMatrix values="0 0 0 0 0  0 1 0 0 0  0 0 0 0 0  0 0 0 1 0" />
      </filter>
      <filter id={`${id}-blue-channel`} colorInterpolationFilters="sRGB">
        <feColorMatrix values="0 0 0 0 0  0 0 0 0 0  0 0 1 0 0  0 0 0 1 0" />
      </filter>
      <filter id={id} x="-3%" y="-3%" width="106%" height="106%" colorInterpolationFilters="sRGB">
      <feImage ref={mapRef} x="0" y="0" preserveAspectRatio="none" result="signalMap" />
      <feComponentTransfer in="signalMap" result="signalCoordinates">
        <feFuncR type="linear" slope={255 / 254} intercept={-1 / 254} />
        <feFuncG type="linear" slope={255 / 254} intercept={-1 / 254} />
      </feComponentTransfer>
      <feDisplacementMap ref={displacementRef} in="SourceGraphic" in2="signalCoordinates" scale="0"
        xChannelSelector="R" yChannelSelector="G" result="signalImage" />
      {shift > 0 && <>
        <feColorMatrix in="signalImage" values="1 0 0 0 0  0 0 0 0 0  0 0 0 0 0  0 0 0 1 0" result="red" />
        <feOffset in="red" dx={-shift} result="redShift" />
        <feColorMatrix in="signalImage" values="0 0 0 0 0  0 1 0 0 0  0 0 0 0 0  0 0 0 1 0" result="green" />
        <feColorMatrix in="signalImage" values="0 0 0 0 0  0 0 0 0 0  0 0 1 0 0  0 0 0 1 0" result="blue" />
        <feOffset in="blue" dx={shift} result="blueShift" />
        <feComposite in="redShift" in2="green" operator="arithmetic" k2="1" k3="1" result="redGreen" />
        <feComposite in="redGreen" in2="blueShift" operator="arithmetic" k2="1" k3="1" />
      </>}
    </filter></defs>
  </svg>
}
