import { useEffect, useRef, useState } from 'react'
import axios from 'axios'
import {
  fetchApps,
  fetchLogicControls,
  fetchConfig,
  fetchLogTail,
  loadConfigFile,
  sendChannelAction,
  sendGlobalLogicAction,
  streamUrl,
  type AppInfo,
  type LogicControlsResponse,
  type LogicActionDef,
} from '../api/client'
import { useAuthStore } from '../store/authStore'
import './LiveViewPage.css'

type RtspState = 'idle' | 'checking' | 'enabled' | 'disabled' | 'error'

const STREAM_MAX_RETRY = 25
const STREAM_STALL_MS = 15000
const STREAM_INIT_LIMIT = 4 * 1024 * 1024

class FatalStreamError extends Error {}

function concatBytes(parts: Uint8Array[], total: number): Uint8Array {
  const merged = new Uint8Array(total)
  let offset = 0
  for (const part of parts) {
    merged.set(part, offset)
    offset += part.byteLength
  }
  return merged
}

/** SourceBuffer 不接受 SharedArrayBuffer；普通 fetch 数据保持零复制，仅截取有效视图范围。 */
function sourceBufferBytes(bytes: Uint8Array<ArrayBufferLike>): ArrayBuffer {
  if (bytes.buffer instanceof ArrayBuffer) {
    if (bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength) return bytes.buffer
    return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength)
  }
  const owned = new Uint8Array(bytes.byteLength)
  owned.set(bytes)
  return owned.buffer
}

/** 从 MP4 初始化段的 avcC box 读取真实 H264 profile/compatibility/level。 */
function findAvcCodec(data: Uint8Array): string | null {
  for (let i = 4; i + 8 <= data.byteLength; i += 1) {
    if (data[i] !== 0x61 || data[i + 1] !== 0x76 || data[i + 2] !== 0x63 || data[i + 3] !== 0x43) continue
    const hex = (value: number) => value.toString(16).padStart(2, '0').toUpperCase()
    return `avc1.${hex(data[i + 5])}${hex(data[i + 6])}${hex(data[i + 7])}`
  }
  return null
}

function waitForSourceOpen(mediaSource: MediaSource): Promise<void> {
  if (mediaSource.readyState === 'open') return Promise.resolve()
  return new Promise((resolve, reject) => {
    const onOpen = () => { cleanup(); resolve() }
    const onClose = () => { cleanup(); reject(new Error('MediaSource 在初始化前关闭')) }
    const cleanup = () => {
      mediaSource.removeEventListener('sourceopen', onOpen)
      mediaSource.removeEventListener('sourceclose', onClose)
    }
    mediaSource.addEventListener('sourceopen', onOpen)
    mediaSource.addEventListener('sourceclose', onClose)
  })
}

function sourceBufferOperation(sourceBuffer: SourceBuffer, operation: () => void): Promise<void> {
  return new Promise((resolve, reject) => {
    const onDone = () => { cleanup(); resolve() }
    const onError = () => { cleanup(); reject(new Error('浏览器视频缓冲区写入失败')) }
    const cleanup = () => {
      sourceBuffer.removeEventListener('updateend', onDone)
      sourceBuffer.removeEventListener('error', onError)
    }
    sourceBuffer.addEventListener('updateend', onDone)
    sourceBuffer.addEventListener('error', onError)
    try {
      operation()
    } catch (error) {
      cleanup()
      reject(error)
    }
  })
}

async function readStreamChunk(
  reader: ReadableStreamDefaultReader<Uint8Array>,
  timeoutMs: number,
): Promise<ReadableStreamReadResult<Uint8Array>> {
  let timer: ReturnType<typeof setTimeout> | null = null
  try {
    return await Promise.race([
      reader.read(),
      new Promise<never>((_, reject) => {
        timer = setTimeout(() => reject(new Error('视频数据接收超时')), timeoutMs)
      }),
    ])
  } finally {
    if (timer) clearTimeout(timer)
  }
}

function errMsg(error: unknown): string {
  if (axios.isAxiosError(error)) {
    return error.response?.data?.detail ?? error.response?.data?.message ?? error.message
  }
  return error instanceof Error ? error.message : String(error)
}

function runtimeConfigPath(configName: string): string | null {
  const name = configName.trim()
  if (!name || name === 'config.json') return null
  return name.startsWith('assets/') ? name : `assets/${name}`
}

export default function LiveViewPage() {
  const [apps, setApps] = useState<AppInfo[]>([])
  const [appsLoading, setAppsLoading] = useState(true)
  const [appsError, setAppsError] = useState('')
  const [rtspState, setRtspState] = useState<RtspState>('idle')
  const [streamErr, setStreamErr] = useState(false)
  const [streamLoading, setStreamLoading] = useState(true)
  const [streamNonce, setStreamNonce] = useState(0)
  const [videoFullscreen, setVideoFullscreen] = useState(false)
  const [streamLogs, setStreamLogs] = useState<string[]>([])
  const [logConnected, setLogConnected] = useState(false)
  const [controls, setControls] = useState<LogicControlsResponse | null>(null)
  const [actionBusy, setActionBusy] = useState<Record<string, boolean>>({})
  const [toast, setToast] = useState<{ msg: string; type: 'ok' | 'err' } | null>(null)

  const streamRetryRef = useRef(0)
  const streamRetryTimer = useRef<ReturnType<typeof setTimeout> | null>(null)
  const streamRetryPendingRef = useRef(false)
  const videoRef = useRef<HTMLVideoElement>(null)
  const videoFrameRef = useRef<HTMLDivElement>(null)
  const logWsRef = useRef<WebSocket | null>(null)
  const logBoxRef = useRef<HTMLDivElement>(null)
  const logAutoScrollRef = useRef(true)
  const pendingLogsRef = useRef<string[]>([])
  const toastTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null)

  const runningApp = apps.find(app => app.status === 'running') ?? null
  const appName = runningApp?.name ?? ''
  const runningConfig = runningApp?.config ?? ''
  const rtspEnabled = rtspState === 'enabled'
  const actionTargets = controls ? [
    ...controls.globals.map(instance => ({
      key: `global:${instance.instance_id}`,
      title: '全局逻辑',
      subtitle: instance.logic_label,
      enabled: instance.enabled,
      actions: instance.actions,
      run: (action: LogicActionDef) => sendGlobalLogicAction(
        appName, instance.instance_id, action.id, action.payload ?? {},
      ),
    })),
    ...controls.channels.map(channel => ({
      key: `channel:${channel.channel_id}`,
      title: `通道 ${channel.channel_id}`,
      subtitle: channel.logic_label,
      enabled: channel.enabled,
      actions: channel.actions,
      run: (action: LogicActionDef) => sendChannelAction(
        appName, channel.channel_id, action.id, action.payload ?? {},
      ),
    })),
  ] : []
  const hasActions = actionTargets.some(target => target.actions.length > 0)

  const showToast = (msg: string, type: 'ok' | 'err' = 'ok') => {
    if (toastTimerRef.current) clearTimeout(toastTimerRef.current)
    setToast({ msg, type })
    toastTimerRef.current = null
    if (type === 'ok') {
      toastTimerRef.current = setTimeout(() => {
        setToast(null)
        toastTimerRef.current = null
      }, 3000)
    }
  }

  const resumeLogScroll = () => {
    logAutoScrollRef.current = true
    const pending = pendingLogsRef.current
    pendingLogsRef.current = []
    if (pending.length > 0) {
      setStreamLogs(previous => [...previous, ...pending].slice(-1000))
    }
    setTimeout(() => {
      const element = logBoxRef.current
      if (element && logAutoScrollRef.current) element.scrollTop = element.scrollHeight
    }, 0)
  }

  const handleLogScroll = () => {
    const element = logBoxRef.current
    if (!element) return
    const nearBottom = element.scrollHeight - element.scrollTop - element.clientHeight < 40
    if (nearBottom) {
      if (!logAutoScrollRef.current || pendingLogsRef.current.length > 0) resumeLogScroll()
    } else {
      logAutoScrollRef.current = false
    }
  }

  const clearStreamRetry = () => {
    if (streamRetryTimer.current) clearTimeout(streamRetryTimer.current)
    streamRetryTimer.current = null
    streamRetryPendingRef.current = false
  }

  const scheduleStreamRetry = (delay: number) => {
    if (streamRetryPendingRef.current) return
    clearStreamRetry()
    if (streamRetryRef.current >= STREAM_MAX_RETRY) {
      setStreamLoading(false)
      setStreamErr(true)
      return
    }
    streamRetryRef.current += 1
    streamRetryPendingRef.current = true
    setStreamLoading(true)
    streamRetryTimer.current = setTimeout(() => {
      streamRetryPendingRef.current = false
      streamRetryTimer.current = null
      setStreamNonce(Date.now())
    }, delay)
  }

  const handleStreamLoad = () => {
    clearStreamRetry()
    streamRetryRef.current = 0
    setStreamLoading(false)
  }

  const retryStream = () => {
    clearStreamRetry()
    streamRetryRef.current = 0
    setStreamErr(false)
    setStreamLoading(true)
    setStreamNonce(Date.now())
  }

  const toggleVideoFullscreen = async () => {
    try {
      if (document.fullscreenElement === videoFrameRef.current) {
        await document.exitFullscreen()
      } else if (videoFrameRef.current) {
        await videoFrameRef.current.requestFullscreen()
      }
    } catch (error) {
      showToast(`切换全屏失败：${errMsg(error)}`, 'err')
    }
  }

  const handleAction = async (
    targetKey: string,
    targetLabel: string,
    action: LogicActionDef,
    run: () => Promise<{ message?: string }>,
  ) => {
    if (!appName) return
    if (action.confirm && !window.confirm(action.confirm)) return
    const key = `${targetKey}:${action.id}`
    setActionBusy(previous => ({ ...previous, [key]: true }))
    try {
      const response = await run()
      showToast(
        response?.message
          ? `${targetLabel}：${response.message}`
          : `${targetLabel}的操作已进入队列`,
      )
    } catch (error) {
      showToast(`${targetLabel}操作失败：${errMsg(error)}`, 'err')
    } finally {
      setActionBusy(previous => ({ ...previous, [key]: false }))
    }
  }

  // 页面始终跟随当前唯一处于 running 状态的视觉程序。
  useEffect(() => {
    let disposed = false
    let requestPending = false
    const loadApps = async () => {
      if (requestPending) return
      requestPending = true
      try {
        const data = await fetchApps()
        if (!disposed) {
          setApps(data)
          setAppsError('')
        }
      } catch (error) {
        if (!disposed) setAppsError(`读取程序状态失败：${errMsg(error)}`)
      } finally {
        requestPending = false
        if (!disposed) setAppsLoading(false)
      }
    }
    loadApps()
    const timer = setInterval(loadApps, 3000)
    return () => {
      disposed = true
      clearInterval(timer)
    }
  }, [])

  // 读取程序真正使用的运行配置。未明确启用 RTSP 时不创建 img 流连接。
  useEffect(() => {
    if (!appName) {
      setRtspState('idle')
      return
    }
    let disposed = false
    setRtspState('checking')
    const checkRtsp = async () => {
      try {
        const path = runtimeConfigPath(runningConfig)
        const config = path ? await loadConfigFile(appName, path) : await fetchConfig(appName)
        if (disposed) return
        if (!config) {
          setRtspState('error')
          return
        }
        const globalConfig = ((config as Record<string, unknown>).global ?? config) as Record<string, unknown>
        setRtspState(Number(globalConfig.enable_rtsp ?? 0) !== 0 ? 'enabled' : 'disabled')
      } catch {
        if (!disposed) setRtspState('error')
      }
    }
    checkRtsp()
    return () => { disposed = true }
  }, [appName, runningConfig])

  // 通道/全局动作定义和控制 socket 状态会在程序刚启动后变化，因此持续刷新。
  useEffect(() => {
    if (!appName) {
      setControls(null)
      setActionBusy({})
      return
    }
    let disposed = false
    const loadControls = async () => {
      try {
        const data = await fetchLogicControls(appName)
        if (!disposed) setControls(data)
      } catch {
        if (!disposed) setControls(null)
      }
    }
    setControls(null)
    setActionBusy({})
    loadControls()
    const timer = setInterval(loadControls, 3000)
    return () => {
      disposed = true
      clearInterval(timer)
    }
  }, [appName])

  // 复用原实时画面弹窗的日志尾部读取和 WebSocket 输出，并在断线后自动重连。
  useEffect(() => {
    if (!appName) {
      setStreamLogs([])
      setLogConnected(false)
      return
    }
    let disposed = false
    let reconnectTimer: ReturnType<typeof setTimeout> | null = null
    logAutoScrollRef.current = true
    pendingLogsRef.current = []
    setStreamLogs([])

    const stickToBottom = () => {
      const element = logBoxRef.current
      if (element && logAutoScrollRef.current) element.scrollTop = element.scrollHeight
    }

    fetchLogTail(appName, 200)
      .then(data => {
        if (disposed) return
        setStreamLogs(Array.isArray(data.lines) ? data.lines : [])
        setTimeout(stickToBottom, 50)
      })
      .catch(() => {})

    const connect = () => {
      if (disposed) return
      const token = useAuthStore.getState().token ?? ''
      const protocol = location.protocol === 'https:' ? 'wss' : 'ws'
      const wsUrl = `${protocol}://${location.host}/ws/logs/${encodeURIComponent(appName)}?token=${encodeURIComponent(token)}`
      const ws = new WebSocket(wsUrl)
      logWsRef.current = ws
      ws.onopen = () => { if (!disposed) setLogConnected(true) }
      ws.onclose = () => {
        if (disposed) return
        setLogConnected(false)
        reconnectTimer = setTimeout(connect, 1500)
      }
      ws.onerror = () => { if (!disposed) setLogConnected(false) }
      ws.onmessage = event => {
        const text = String(event.data)
        if (!text) return
        const added = text.split('\n').filter(line => line !== '')
        if (added.length === 0) return
        if (logAutoScrollRef.current) {
          setStreamLogs(previous => [...previous, ...added].slice(-1000))
          setTimeout(stickToBottom, 10)
        } else {
          pendingLogsRef.current = [...pendingLogsRef.current, ...added].slice(-5000)
        }
      }
    }
    connect()

    return () => {
      disposed = true
      if (reconnectTimer) clearTimeout(reconnectTimer)
      logWsRef.current?.close()
      logWsRef.current = null
      pendingLogsRef.current = []
      setLogConnected(false)
    }
  }, [appName])

  // 只有运行配置明确开启 RTSP 时才开始拉流；程序停止或关闭 RTSP 会立即清理重试。
  useEffect(() => {
    clearStreamRetry()
    streamRetryRef.current = 0
    setStreamErr(false)
    setStreamLoading(true)
    return clearStreamRetry
  }, [appName, rtspEnabled])

  useEffect(() => {
    const syncFullscreenState = () => {
      setVideoFullscreen(document.fullscreenElement === videoFrameRef.current)
    }
    document.addEventListener('fullscreenchange', syncFullscreenState)
    return () => document.removeEventListener('fullscreenchange', syncFullscreenState)
  }, [])

  // H264 fMP4 通过 MSE 直接交给浏览器硬解。此处只维护增量缓冲和实时点，
  // 不在前端解析视频帧，也不保留 MJPEG/图片播放回退。
  useEffect(() => {
    if (!appName || !rtspEnabled || streamErr) return
    const video = videoRef.current
    if (!video) return
    if (!('MediaSource' in window)) {
      setStreamLoading(false)
      setStreamErr(true)
      showToast('当前浏览器不支持 Media Source Extensions，无法进行 H264 零转码播放', 'err')
      return
    }

    let disposed = false
    const abortController = new AbortController()
    const mediaSource = new MediaSource()
    const objectUrl = URL.createObjectURL(mediaSource)
    let sourceBuffer: SourceBuffer | null = null
    let reader: ReadableStreamDefaultReader<Uint8Array> | null = null

    video.src = objectUrl
    video.muted = true
    let playbackStarted = false

    const append = async (bytes: Uint8Array) => {
      if (!sourceBuffer || mediaSource.readyState !== 'open' || disposed) return

      // 只保留实时点附近的数据，避免长时间打开页面后触发 MSE QuotaExceededError。
      if (sourceBuffer.buffered.length > 0) {
        const bufferedStart = sourceBuffer.buffered.start(0)
        const keepFrom = video.currentTime - 5
        if (video.currentTime - bufferedStart > 8 && keepFrom > bufferedStart) {
          await sourceBufferOperation(sourceBuffer, () => sourceBuffer!.remove(bufferedStart, keepFrom))
        }
      }
      await sourceBufferOperation(sourceBuffer, () => sourceBuffer!.appendBuffer(sourceBufferBytes(bytes)))

      if (sourceBuffer.buffered.length > 0) {
        const liveEdge = sourceBuffer.buffered.end(sourceBuffer.buffered.length - 1)
        // 保留约两个25FPS视频帧的解码余量；落后超过600ms时主动追赶实时点。
        if (!Number.isFinite(video.currentTime) || liveEdge - video.currentTime > 0.6) {
          video.currentTime = Math.max(0, liveEdge - 0.08)
        }
      }
      if (video.paused) void video.play().catch(() => {})
    }

    const start = async () => {
      await waitForSourceOpen(mediaSource)
      const token = useAuthStore.getState().token ?? ''
      const response = await fetch(`${streamUrl(appName)}?t=${streamNonce}`, {
        headers: token ? { Authorization: `Bearer ${token}` } : undefined,
        cache: 'no-store',
        signal: abortController.signal,
      })
      if (!response.ok) {
        let detail = `视频服务返回 HTTP ${response.status}`
        try {
          const body = await response.json() as { detail?: string }
          if (body.detail) detail = body.detail
        } catch { /* 非 JSON 错误响应 */ }
        if (response.status === 409) throw new FatalStreamError(detail)
        throw new Error(detail)
      }
      if (!response.body) throw new Error('浏览器没有收到视频响应体')
      reader = response.body.getReader()

      // SourceBuffer 必须使用视频真实的 AVC codec string；从初始化段 avcC 中读取，
      // 避免把摄像头的 Main/High Profile 错报成固定 Baseline。
      const initialParts: Uint8Array[] = []
      let initialSize = 0
      let codec: string | null = null
      let initialBytes: Uint8Array | null = null
      while (!codec) {
        const result = await readStreamChunk(reader, STREAM_STALL_MS)
        if (result.done || !result.value) throw new Error('视频流在初始化完成前中断')
        initialParts.push(result.value)
        initialSize += result.value.byteLength
        if (initialSize > STREAM_INIT_LIMIT) throw new Error('视频初始化段异常：未找到 H264 avcC')
        initialBytes = concatBytes(initialParts, initialSize)
        codec = findAvcCodec(initialBytes)
      }

      const mime = `video/mp4; codecs="${codec}"`
      if (!MediaSource.isTypeSupported(mime)) {
        throw new FatalStreamError(`当前浏览器不支持该 H264 格式：${codec}`)
      }
      sourceBuffer = mediaSource.addSourceBuffer(mime)
      sourceBuffer.mode = 'segments'
      await append(initialBytes!)

      while (!disposed) {
        const result = await readStreamChunk(reader, STREAM_STALL_MS)
        if (result.done) throw new Error('视频流已中断')
        if (result.value?.byteLength) await append(result.value)
      }
    }

    start().catch(error => {
      if (disposed || abortController.signal.aborted) return
      setStreamLoading(false)
      if (error instanceof FatalStreamError) {
        setStreamErr(true)
        showToast(error.message, 'err')
      } else {
        scheduleStreamRetry(500)
      }
    })

    const onVideoError = () => {
      if (!disposed) {
        scheduleStreamRetry(500)
        abortController.abort()
      }
    }
    const onVideoPlaying = () => { playbackStarted = true }
    video.addEventListener('error', onVideoError)
    video.addEventListener('playing', onVideoPlaying)
    const startupTimer = setTimeout(() => {
      if (!disposed && !playbackStarted) {
        scheduleStreamRetry(500)
        abortController.abort()
      }
    }, STREAM_STALL_MS)
    let lastPlaybackTime = 0
    let lastPlaybackAdvanceAt = Date.now()
    const playbackWatchdog = setInterval(() => {
      if (disposed || document.hidden || !playbackStarted) {
        lastPlaybackAdvanceAt = Date.now()
        lastPlaybackTime = video.currentTime
        return
      }
      if (video.currentTime > lastPlaybackTime + 0.05) {
        lastPlaybackTime = video.currentTime
        lastPlaybackAdvanceAt = Date.now()
      } else if (Date.now() - lastPlaybackAdvanceAt > STREAM_STALL_MS) {
        scheduleStreamRetry(500)
        abortController.abort()
      }
    }, 2500)

    return () => {
      disposed = true
      clearTimeout(startupTimer)
      clearInterval(playbackWatchdog)
      abortController.abort()
      reader?.cancel().catch(() => {})
      video.removeEventListener('error', onVideoError)
      video.removeEventListener('playing', onVideoPlaying)
      try {
        if (mediaSource.readyState === 'open') mediaSource.endOfStream()
      } catch { /* 流已关闭 */ }
      video.pause()
      video.removeAttribute('src')
      video.load()
      URL.revokeObjectURL(objectUrl)
    }
  }, [appName, rtspEnabled, streamNonce, streamErr]) // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => () => {
    clearStreamRetry()
    if (toastTimerRef.current) clearTimeout(toastTimerRef.current)
  }, [])

  const statusText = !runningApp
    ? '未运行'
    : rtspState === 'checking'
      ? '检测配置中'
      : rtspState === 'disabled'
        ? 'RTSP 未开启'
        : rtspState === 'error'
          ? '配置读取失败'
          : streamErr
            ? '视频异常'
            : streamLoading
              ? '正在连接'
              : '实时'

  const statusClass = runningApp && rtspEnabled && !streamErr
    ? (streamLoading ? 'pending' : 'online')
    : runningApp ? 'warning' : 'offline'

  return (
    <div className="live-view-page">
      <header className="live-view-page-header">
        <div>
          <h2>实时画面</h2>
          <p>自动跟随当前运行的视觉程序，保留画面控制和终端输出。</p>
        </div>
        <div className="live-view-runtime">
          {runningApp && <span className="live-view-app-name">{runningApp.name}</span>}
          <span className={`live-view-status ${statusClass}`}>{statusText}</span>
        </div>
      </header>

      {toast && (
        <div className={`live-view-toast ${toast.type}`}>
          <span>{toast.msg}</span>
          {toast.type === 'err' && <button onClick={() => setToast(null)}>×</button>}
        </div>
      )}

      {appsLoading ? (
        <div className="live-view-empty">
          <div className="live-view-empty-icon">◌</div>
          <h3>正在读取程序状态……</h3>
        </div>
      ) : !runningApp ? (
        <div className="live-view-empty">
          <div className="live-view-empty-icon">▶</div>
          <h3>当前没有正在运行的视觉程序</h3>
          <p>程序启动后，这里会自动连接它的实时画面和终端输出。</p>
          {appsError && <p className="live-view-empty-error">{appsError}</p>}
        </div>
      ) : (
        <div className="live-view-workspace">
          <section className="live-view-visual-panel">
            <div className="live-view-video-frame" ref={videoFrameRef}>
              <button
                className="live-view-fullscreen-btn"
                onClick={toggleVideoFullscreen}
                title={videoFullscreen ? '退出全屏（Esc）' : '全屏显示视频'}
              >
                <span aria-hidden="true">{videoFullscreen ? '⊠' : '⛶'}</span>
                {videoFullscreen ? '退出全屏' : '全屏'}
              </button>
              {rtspEnabled ? (
                !streamErr ? (
                  <>
                    <video
                      ref={videoRef}
                      key={`${appName}-${streamNonce}`}
                      className="live-view-video"
                      style={streamLoading ? { visibility: 'hidden' } : undefined}
                      muted
                      autoPlay
                      playsInline
                      onPlaying={handleStreamLoad}
                    />
                    {streamLoading && (
                      <div className="live-view-loading">
                        <div className="live-view-spinner" />
                        <span>正在连接实时画面……</span>
                      </div>
                    )}
                  </>
                ) : (
                  <div className="live-view-video-state error">
                    <strong>实时视频暂不可用</strong>
                    <span>请检查程序推流状态和视频源连接。</span>
                    <button onClick={retryStream}>重新连接</button>
                  </div>
                )
              ) : (
                <div className="live-view-video-state">
                  <strong>
                    {rtspState === 'checking'
                      ? '正在检查运行配置'
                      : rtspState === 'disabled'
                        ? '当前程序未开启 RTSP 推流'
                        : '无法确认 RTSP 推流配置'}
                  </strong>
                  <span>
                    {rtspState === 'disabled'
                      ? '不会建立视频连接；右侧终端输出和自定义功能仍可正常使用。'
                      : '读取到明确启用的 RTSP 配置后才会自动显示画面。'}
                  </span>
                </div>
              )}
            </div>
          </section>

          <div className="live-view-side-column">
            <section className="live-view-console-panel">
              <div className="live-view-panel-head">
                <span>终端输出</span>
                <div className="live-view-console-tools">
                  <span className={`live-view-log-status ${logConnected ? 'online' : ''}`}>
                    {logConnected ? '● 实时' : '○ 重连中'}
                  </span>
                  <button onClick={resumeLogScroll}>跳到底部</button>
                </div>
              </div>
              <div className="live-view-console" ref={logBoxRef} onScroll={handleLogScroll}>
                {streamLogs.length === 0 ? (
                  <div className="live-view-console-empty">暂无终端输出。</div>
                ) : streamLogs.map((line, index) => (
                  <div
                    key={index}
                    className={`live-view-log-line${/ERROR|error|\[进程已停止\]/.test(line) ? ' error' : /WARN/.test(line) ? ' warning' : ''}`}
                  >
                    {line}
                  </div>
                ))}
              </div>
            </section>

            <section className="live-view-controls-panel">
              <div className="live-view-panel-head">
                <span>自定义功能</span>
                {controls && !controls.socket_ready && (
                  <span className="live-view-control-badge">控制通道未连接</span>
                )}
              </div>
              <div className="live-view-controls">
                {!controls ? (
                  <div className="live-view-control-empty">暂时无法获取通道控制信息。</div>
                ) : !hasActions ? (
                  <div className="live-view-control-empty">当前程序没有可用的自定义功能。</div>
                ) : (
                  <>
                    <div className="live-view-control-grid">
                      {actionTargets.filter(target => target.actions.length > 0).map(target => (
                        <div key={target.key} className="live-view-control-card">
                          <div className="live-view-control-title">
                            <span>{target.title}</span>
                            <span>{target.subtitle}</span>
                          </div>
                          <div className="live-view-control-actions">
                            {target.actions.map(action => {
                              const key = `${target.key}:${action.id}`
                              const disabled = !controls.socket_ready || !target.enabled || !!actionBusy[key]
                              return (
                                <button
                                  key={key}
                                  className={`live-view-action ${action.style ?? 'default'}`}
                                  disabled={disabled}
                                  title={action.help ?? action.id}
                                  onClick={() => handleAction(
                                    target.key,
                                    target.title,
                                    action,
                                    () => target.run(action),
                                  )}
                                >
                                  {actionBusy[key] ? '处理中…' : (action.label ?? action.id)}
                                </button>
                              )
                            })}
                          </div>
                        </div>
                      ))}
                    </div>
                  </>
                )}
              </div>
            </section>
          </div>
        </div>
      )}
    </div>
  )
}
