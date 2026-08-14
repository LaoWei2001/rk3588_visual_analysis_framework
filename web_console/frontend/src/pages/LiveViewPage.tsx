import { useEffect, useRef, useState } from 'react'
import axios from 'axios'
import {
  fetchApps,
  fetchChannelControls,
  fetchConfig,
  fetchLogTail,
  fetchStreamHealth,
  loadConfigFile,
  sendChannelAction,
  streamUrl,
  type AppInfo,
  type ChannelControlsResponse,
  type LogicActionDef,
} from '../api/client'
import { useAuthStore } from '../store/authStore'
import './LiveViewPage.css'

type RtspState = 'idle' | 'checking' | 'enabled' | 'disabled' | 'error'

const STREAM_MAX_RETRY = 25
const STREAM_STALL_MS = 4000

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
  const [controls, setControls] = useState<ChannelControlsResponse | null>(null)
  const [actionBusy, setActionBusy] = useState<Record<string, boolean>>({})
  const [toast, setToast] = useState<{ msg: string; type: 'ok' | 'err' } | null>(null)

  const streamRetryRef = useRef(0)
  const streamRetryTimer = useRef<ReturnType<typeof setTimeout> | null>(null)
  const streamRetryPendingRef = useRef(false)
  const streamLoadingRef = useRef(true)
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
  const hasActions = controls?.channels.some(channel => channel.actions.length > 0) ?? false

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

  const handleAction = async (channelId: number, action: LogicActionDef) => {
    if (!appName) return
    if (action.confirm && !window.confirm(action.confirm)) return
    const key = `${channelId}:${action.id}`
    setActionBusy(previous => ({ ...previous, [key]: true }))
    try {
      const response = await sendChannelAction(appName, channelId, action.id, action.payload ?? {})
      showToast(
        response?.message
          ? `通道 ${channelId}：${response.message}`
          : `通道 ${channelId} 的操作已进入队列`,
      )
    } catch (error) {
      showToast(`通道 ${channelId} 操作失败：${errMsg(error)}`, 'err')
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

  // 通道动作定义和控制 socket 状态会在程序刚启动后变化，因此持续刷新。
  useEffect(() => {
    if (!appName) {
      setControls(null)
      setActionBusy({})
      return
    }
    let disposed = false
    const loadControls = async () => {
      try {
        const data = await fetchChannelControls(appName)
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
    if (appName && rtspEnabled) setStreamNonce(Date.now())
    return clearStreamRetry
  }, [appName, rtspEnabled])

  useEffect(() => { streamLoadingRef.current = streamLoading }, [streamLoading])

  useEffect(() => {
    const syncFullscreenState = () => {
      setVideoFullscreen(document.fullscreenElement === videoFrameRef.current)
    }
    document.addEventListener('fullscreenchange', syncFullscreenState)
    return () => document.removeEventListener('fullscreenchange', syncFullscreenState)
  }, [])

  // MJPEG 连接没有触发 onLoad/onError 时，以首帧看门狗主动重连。
  useEffect(() => {
    if (!appName || !rtspEnabled || streamErr) return
    const timer = setTimeout(() => {
      if (streamLoadingRef.current) scheduleStreamRetry(0)
    }, STREAM_STALL_MS)
    return () => clearTimeout(timer)
  }, [appName, rtspEnabled, streamNonce, streamErr]) // eslint-disable-line react-hooks/exhaustive-deps

  // 首帧后继续复用后端帧心跳，避免 MJPEG 长连接中途卡死却不触发 img.onError。
  useEffect(() => {
    if (!appName || !rtspEnabled || streamErr) return
    let disposed = false
    const checkHealth = async () => {
      try {
        const health = await fetchStreamHealth(appName)
        if (disposed || streamLoadingRef.current) return
        const stalled = !health.active || health.last_data_age_ms == null || health.last_data_age_ms > 10000
        if (stalled) scheduleStreamRetry(0)
      } catch {
        // 单次健康检查失败不打断当前画面，下一轮继续确认。
      }
    }
    const timer = setInterval(checkHealth, 2500)
    return () => {
      disposed = true
      clearInterval(timer)
    }
  }, [appName, rtspEnabled, streamErr]) // eslint-disable-line react-hooks/exhaustive-deps

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
                    <img
                      key={`${appName}-${streamNonce}`}
                      className="live-view-image"
                      style={streamLoading ? { visibility: 'hidden' } : undefined}
                      src={`${streamUrl(appName)}&t=${streamNonce}`}
                      alt={`${appName} 实时画面`}
                      onLoad={handleStreamLoad}
                      onError={() => scheduleStreamRetry(1500)}
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
                      {controls.channels.filter(channel => channel.actions.length > 0).map(channel => (
                        <div key={channel.channel_id} className="live-view-control-card">
                          <div className="live-view-control-title">
                            <span>通道 {channel.channel_id}</span>
                            <span>{channel.logic_label}</span>
                          </div>
                          <div className="live-view-control-actions">
                            {channel.actions.map(action => {
                              const key = `${channel.channel_id}:${action.id}`
                              const disabled = !controls.socket_ready || !channel.enabled || !!actionBusy[key]
                              return (
                                <button
                                  key={key}
                                  className={`live-view-action ${action.style ?? 'default'}`}
                                  disabled={disabled}
                                  title={action.help ?? action.id}
                                  onClick={() => handleAction(channel.channel_id, action)}
                                >
                                  {actionBusy[key] ? '处理中…' : (action.label ?? action.id)}
                                </button>
                              )
                            })}
                          </div>
                        </div>
                      ))}
                    </div>
                    <p className="live-view-control-tip">按钮操作按通道进入队列，并在下一帧通道逻辑执行前处理。</p>
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
