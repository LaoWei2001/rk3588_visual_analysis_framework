// The latest continuous range is the only valid destination after a gap.
export const LIVE_START_BUFFER_SECONDS = 0.4
export const LIVE_TARGET_LATENCY_SECONDS = 0.4
export const LIVE_MAX_LATENCY_SECONDS = 1

export function livePlaybackTarget(currentTime: number, start: number, end: number, started: boolean): number | null {
  if (![currentTime, start, end].every(Number.isFinite) || end <= start) return null
  if (!started) {
    return end - start >= LIVE_START_BUFFER_SECONDS ? Math.max(start, end - LIVE_TARGET_LATENCY_SECONDS) : null
  }
  return currentTime < start || end - currentTime > LIVE_MAX_LATENCY_SECONDS
    ? Math.max(start, end - LIVE_TARGET_LATENCY_SECONDS) : null
}
