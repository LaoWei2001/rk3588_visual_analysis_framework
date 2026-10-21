// The latest continuous range is the only valid destination after a gap.
export const LIVE_START_BUFFER_SECONDS = 0.6
export const LIVE_TARGET_LATENCY_SECONDS = 0.6
export const LIVE_MAX_LATENCY_SECONDS = 3

export function livePlaybackTarget(currentTime: number, start: number, end: number, started: boolean,
  targetLatency = LIVE_TARGET_LATENCY_SECONDS): number | null {
  if (![currentTime, start, end].every(Number.isFinite) || end <= start) return null
  if (!started) {
    return end - start >= Math.max(LIVE_START_BUFFER_SECONDS, targetLatency) ? Math.max(start, end - targetLatency) : null
  }
  return currentTime < start - 0.05 || end - currentTime > LIVE_MAX_LATENCY_SECONDS
    ? Math.max(start, end - targetLatency) : null
}

// 小幅积压通过轻微加速消化，避免每次网络/渲染抖动都 seek 并重新等待关键帧。
export function livePlaybackRate(currentTime: number, end: number, targetLatency = LIVE_TARGET_LATENCY_SECONDS): number {
  return Number.isFinite(end - currentTime) && end - currentTime > Math.max(1.2, targetLatency + 0.6) ? 1.05 : 1
}

// Keep the last decoded frame while accumulating a modest reserve after real
// starvation. Repeated waiting/playing oscillations otherwise empty a thin
// buffer immediately, particularly with two decoders competing on one PC.
export class LivePlaybackBuffer {
  targetSeconds = LIVE_TARGET_LATENCY_SECONDS
  rebuffering = false

  starved(ahead: number): boolean {
    if (this.rebuffering || !Number.isFinite(ahead) || ahead > 0.25) return false
    this.targetSeconds = Math.min(1.8, this.targetSeconds + 0.4)
    this.rebuffering = true
    return true
  }

  resume(ahead: number): boolean {
    if (!this.rebuffering || !Number.isFinite(ahead) || ahead < this.targetSeconds) return false
    this.rebuffering = false
    return true
  }
}
