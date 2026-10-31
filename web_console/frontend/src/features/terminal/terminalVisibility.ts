import { useEffect } from 'react'
import type { RefObject } from 'react'

export const EFFECTS_VISIBILITY_EVENT = 'terminal-effects-visibility'
export const effectsPaused = (surface: HTMLElement) => surface.parentElement?.dataset.effectsPaused === 'true'

/** Visible CRTs animate continuously, even when the shell has no new output. */
export function useTerminalVisibility(surfaceRef: RefObject<HTMLDivElement>, enabled: boolean) {
  useEffect(() => {
    const surface = surfaceRef.current
    const screen = surface?.parentElement
    if (!surface || !screen || !enabled) return
    const reduced = matchMedia('(prefers-reduced-motion: reduce)')
    let intersecting = true
    const visibility = () => {
      const paused = document.hidden || !intersecting || reduced.matches
      if (screen.dataset.effectsPaused === String(paused)) return
      screen.dataset.effectsPaused = String(paused)
      surface.dispatchEvent(new Event(EFFECTS_VISIBILITY_EVENT))
    }
    const intersection = new IntersectionObserver(entries => {
      const next = entries[0]?.isIntersecting ?? true
      if (next !== intersecting) { intersecting = next; visibility() }
    })
    intersection.observe(screen)
    document.addEventListener('visibilitychange', visibility)
    reduced.addEventListener('change', visibility)
    visibility()
    return () => {
      intersection.disconnect()
      document.removeEventListener('visibilitychange', visibility)
      reduced.removeEventListener('change', visibility)
      delete screen.dataset.effectsPaused
    }
  }, [surfaceRef, enabled])
}
