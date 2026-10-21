type OperationPhase = 'source-open' | 'buffer' | 'read'

export class StreamOperationTimeout extends Error {
  constructor(readonly phase: OperationPhase) {
    const message = { 'source-open': '浏览器视频源初始化超时', buffer: '浏览器视频缓冲操作超时', read: '视频数据接收超时' }
    super(message[phase])
    this.name = 'StreamOperationTimeout'
  }
}

const abortError = () => new DOMException('视频操作已取消', 'AbortError')

function waitForMediaEvent(
  target: EventTarget, success: string, failures: string[], signal: AbortSignal,
  timeoutMs: number, phase: OperationPhase, operation: () => void,
): Promise<void> {
  return new Promise((resolve, reject) => {
    if (signal.aborted) { reject(abortError()); return }
    let timer: ReturnType<typeof setTimeout> | null = null
    const cleanup = () => {
      if (timer !== null) clearTimeout(timer)
      target.removeEventListener(success, onDone)
      for (const event of failures) target.removeEventListener(event, onError)
      signal.removeEventListener('abort', onAbort)
    }
    const onDone = () => { cleanup(); resolve() }
    const onError = () => { cleanup(); reject(new Error(phase === 'buffer' ? '浏览器视频缓冲操作失败' : '浏览器视频源已关闭')) }
    const onAbort = () => { cleanup(); reject(abortError()) }
    target.addEventListener(success, onDone)
    for (const event of failures) target.addEventListener(event, onError)
    signal.addEventListener('abort', onAbort, { once: true })
    timer = setTimeout(() => { cleanup(); reject(new StreamOperationTimeout(phase)) }, timeoutMs)
    try { operation() } catch (error) { cleanup(); reject(error) }
  })
}

export function waitForSourceOpen(media: MediaSource, signal: AbortSignal, timeoutMs = 10000): Promise<void> {
  if (signal.aborted) return Promise.reject(abortError())
  if (media.readyState === 'open') return Promise.resolve()
  return waitForMediaEvent(media, 'sourceopen', ['sourceclose'], signal, timeoutMs, 'source-open', () => {})
}

export function sourceBufferOperation(
  source: SourceBuffer, operation: () => void, signal: AbortSignal, timeoutMs = 5000,
): Promise<void> {
  return waitForMediaEvent(source, 'updateend', ['error', 'abort'], signal, timeoutMs, 'buffer', operation)
}

export function readStreamChunk(
  reader: ReadableStreamDefaultReader<Uint8Array>, timeoutMs: number, signal: AbortSignal,
): Promise<ReadableStreamReadResult<Uint8Array>> {
  return new Promise((resolve, reject) => {
    if (signal.aborted) { reject(abortError()); return }
    const cleanup = () => { clearTimeout(timer); signal.removeEventListener('abort', onAbort) }
    const onAbort = () => { cleanup(); reject(abortError()) }
    const timer = setTimeout(() => {
      cleanup()
      reject(new StreamOperationTimeout('read'))
      void reader.cancel().catch(() => {})
    }, timeoutMs)
    signal.addEventListener('abort', onAbort, { once: true })
    reader.read().then(result => { cleanup(); resolve(result) }, error => { cleanup(); reject(error) })
  })
}

// Equal jitter keeps retries spread out when several viewers lose the source.
export function streamRetryDelay(attempt: number, random = Math.random()): number {
  const ceiling = Math.min(15000, 500 * 2 ** Math.min(6, Math.max(0, attempt - 1)))
  return Math.round(ceiling * (0.75 + 0.25 * Math.min(1, Math.max(0, random))))
}

export class PlaybackProgress {
  private lastAdvanceAt: number
  private lastSampleAt: number
  private stableSince: number | null = null
  private lastTime: number
  private lastFrames: number | null

  constructor(now: number, time: number, frames: number | null, private readonly stallMs = 15000) {
    this.lastAdvanceAt = this.lastSampleAt = now
    this.lastTime = time
    this.lastFrames = frames
  }

  reset(now: number, time: number, frames: number | null) {
    this.lastAdvanceAt = this.lastSampleAt = now
    this.lastTime = time
    this.lastFrames = frames
    this.stableSince = null
  }

  sample(now: number, time: number, frames: number | null, seeking: boolean, paused: boolean) {
    // On old browsers, reject seek-sized time jumps instead of counting them as
    // playback. Modern browsers use frame callbacks or presented-frame counts.
    const advanced = frames !== null && this.lastFrames !== null ? frames > this.lastFrames
      : time > this.lastTime + 0.01 && time - this.lastTime <= Math.max(0.5, (now - this.lastSampleAt) / 500)
    this.lastTime = time
    this.lastFrames = frames
    this.lastSampleAt = now
    if (seeking || paused || now - this.lastAdvanceAt > 1500) this.stableSince = null
    if (advanced && !seeking && !paused) {
      this.lastAdvanceAt = now
      if (this.stableSince === null) this.stableSince = now
    }
    return { stalled: now - this.lastAdvanceAt > this.stallMs,
      stable: advanced && !seeking && !paused && this.stableSince !== null && now - this.stableSince >= 10000 }
  }
}
