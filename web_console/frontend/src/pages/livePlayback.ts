// The latest continuous range is the only valid destination after a gap.
export const LIVE_START_BUFFER_SECONDS = 0.1
export const LIVE_TARGET_LATENCY_SECONDS = 0.1
export const LIVE_MAX_LATENCY_SECONDS = 0.5
export const LIVE_MAX_RECOVERY_BUFFER_SECONDS = 0.3

export function livePlaybackTarget(currentTime: number, start: number, end: number, started: boolean,
  targetLatency = LIVE_TARGET_LATENCY_SECONDS): number | null {
  if (![currentTime, start, end].every(Number.isFinite) || end <= start) return null
  if (!started) {
    return end - start >= Math.max(LIVE_START_BUFFER_SECONDS, targetLatency) ? Math.max(start, end - targetLatency) : null
  }
  return currentTime < start - 0.05 || end - currentTime > LIVE_MAX_LATENCY_SECONDS
    ? Math.max(start, end - targetLatency) : null
}

// Correct small drift before it becomes seconds of permanent delay. Normal
// fragment arrival jitter stays inside the dead band and does not trigger seek.
export function livePlaybackRate(currentTime: number, end: number, targetLatency = LIVE_TARGET_LATENCY_SECONDS): number {
  const drift = end - currentTime - targetLatency
  if (!Number.isFinite(drift) || drift <= 0.05) return 1
  return drift > 0.15 ? 1.12 : 1.08
}

// Keep the last decoded frame while accumulating a modest reserve after real
// starvation. This reserve is temporary: stable frame presentation brings it
// back down instead of leaving the viewer permanently 1.8 seconds behind.
export class LivePlaybackBuffer {
  targetSeconds = LIVE_TARGET_LATENCY_SECONDS
  rebuffering = false
  private stableSince: number | null = null
  private lastDecreaseAt = 0

  starved(ahead: number): boolean {
    if (this.rebuffering || !Number.isFinite(ahead) || ahead > 0.1) return false
    this.targetSeconds = Math.min(LIVE_MAX_RECOVERY_BUFFER_SECONDS,
      Math.round((this.targetSeconds + 0.1) * 1000) / 1000)
    this.stableSince = null
    this.rebuffering = true
    return true
  }

  resume(ahead: number): boolean {
    if (!this.rebuffering || !Number.isFinite(ahead) || ahead < this.targetSeconds) return false
    this.rebuffering = false
    return true
  }

  observePlayback(now: number, advancing: boolean): void {
    if (!Number.isFinite(now) || !advancing || this.rebuffering) {
      this.stableSince = null
      return
    }
    if (this.stableSince === null || now < this.stableSince) {
      this.stableSince = now
      this.lastDecreaseAt = now
      return
    }
    // Require five seconds of uninterrupted presentation, then remove 100 ms
    // every two seconds. Pauses, seeks and background tabs reset this timer.
    if (now - this.stableSince >= 5000 && now - this.lastDecreaseAt >= 2000) {
      this.targetSeconds = Math.max(LIVE_TARGET_LATENCY_SECONDS,
        Math.round((this.targetSeconds - 0.1) * 1000) / 1000)
      this.lastDecreaseAt = now
    }
  }
}
