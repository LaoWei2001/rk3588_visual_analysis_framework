import { useEffect, useRef, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import axios from 'axios'
import { fetchApps, fetchLogTail, fetchStreamHealth, startApp, stopApp, setAppAutostart, streamUrl, uploadApp, deleteApp, fetchConfig, loadConfigFile, fetchChannelControls, sendChannelAction, AppInfo, ChannelControlsResponse, LogicActionDef } from '../api/client'
import { loadLastConfig } from '../utils/lastConfig'
import { useAuthStore } from '../store/authStore'
import './AppsPage.css'

function errMsg(e: unknown): string {
  if (axios.isAxiosError(e)) {
    return e.response?.data?.detail ?? e.response?.data?.message ?? e.message
  }
  return e instanceof Error ? e.message : String(e)
}

function fmtUptime(s: number | null): string {
  if (s == null) return '-'
  const h = Math.floor(s / 3600)
  const m = Math.floor((s % 3600) / 60)
  const sec = s % 60
  if (h > 0) return `${h}h ${m}m`
  if (m > 0) return `${m}m ${sec}s`
  return `${sec}s`
}

// 下拉框只显示配置文件名，并与 active_config/运行配置保持一致。
const cfgName = (p: string): string => p.split('/').pop() ?? p

export default function AppsPage() {
  const [apps, setApps]       = useState<AppInfo[]>([])
  const [loading, setLoading] = useState(true)
  const [modes, setModes]     = useState<Record<string, 'deploy' | 'debug'>>({})
  const [cfgSel, setCfgSel]   = useState<Record<string, string>>({})   // 每个程序选中的启动配置文件名
  const [busy, setBusy]       = useState<Record<string, boolean>>({})
  const [autostartBusy, setAutostartBusy] = useState<Record<string, boolean>>({})
  const [toast, setToast]     = useState<{ msg: string; type: 'ok' | 'err' } | null>(null)
  const [crashInfo, setCrashInfo] = useState<{ name: string; lines: string[] } | null>(null)
  const [viewApp, setViewApp]       = useState<string | null>(null)   // 正在查看实时画面的程序
  const [streamErr, setStreamErr]   = useState(false)
  const [streamLoading, setStreamLoading] = useState(true)           // 视频首帧到达前显示加载动画
  const [streamLogs, setStreamLogs] = useState<string[]>([])          // 实时画面弹窗右侧的滚动日志
  const [viewNonce, setViewNonce]   = useState(0)                     // 强制刷新视频地址，避免残留上一条流
  const streamRetryRef   = useRef(0)                                  // 视频流尚未就绪时的自动重试次数
  const streamRetryTimer = useRef<ReturnType<typeof setTimeout> | null>(null)
  const streamRetryPendingRef = useRef(false)
  const streamLoadingRef = useRef(true)                               // 供卡流看门狗读取最新加载状态
  const logWsRef  = useRef<WebSocket | null>(null)
  const logBoxRef = useRef<HTMLDivElement>(null)
  const pendingStreamLogsRef = useRef<string[]>([])
  // 日志距底部 40px 内自动跟随；向上滚动时冻结当前列表，新日志先进入缓冲区。
  const logAutoScrollRef = useRef(true)
  const resumeStreamLogScroll = () => {
    logAutoScrollRef.current = true
    const pending = pendingStreamLogsRef.current
    pendingStreamLogsRef.current = []
    if (pending.length > 0) {
      setStreamLogs(prev => [...prev, ...pending].slice(-1000))
    }
    setTimeout(() => {
      const el = logBoxRef.current
      if (el && logAutoScrollRef.current) el.scrollTop = el.scrollHeight
    }, 0)
  }
  const onStreamLogScroll = () => {
    const el = logBoxRef.current
    if (!el) return
    const nearBottom = el.scrollHeight - el.scrollTop - el.clientHeight < 40
    if (nearBottom) {
      if (!logAutoScrollRef.current || pendingStreamLogsRef.current.length > 0) {
        resumeStreamLogScroll()
      }
    } else {
      logAutoScrollRef.current = false
    }
  }
  const fileRef   = useRef<HTMLInputElement>(null)
  const toastTimer = useRef<ReturnType<typeof setTimeout> | null>(null)
  const [uploading, setUploading] = useState<{ name: string; pct: number } | null>(null)
  const [viewControls, setViewControls] = useState<ChannelControlsResponse | null>(null)
  const [channelActionBusy, setChannelActionBusy] = useState<Record<string, boolean>>({})
  const navigate = useNavigate()

  // 打开实时画面前检查 RTSP 配置，避免用户进入后只看到黑屏。
  const openView = async (app: AppInfo) => {
    try {
      const running = app.config && app.config !== 'config.json' ? `assets/${app.config}` : null
      const cfg = running ? await loadConfigFile(app.name, running) : await fetchConfig(app.name)
      const g = (cfg && ((cfg as Record<string, unknown>).global ?? cfg)) as Record<string, unknown> | null
      // 仅在配置明确关闭 RTSP 时拦截；读取失败时仍允许打开。
      if (g && !Number(g.enable_rtsp ?? 0)) {
        showToast(`${app.name} 的配置未启用 RTSP 推流`, 'err')
        return
      }
    } catch { /* 配置读取失败时不拦截，继续使用弹窗内的错误提示。 */ }
    let controls: ChannelControlsResponse | null = null
    try {
      controls = await fetchChannelControls(app.name)
    } catch {
      controls = null
    }
    setViewControls(controls)
    setChannelActionBusy({})
    setStreamErr(false); setStreamLoading(true); streamRetryRef.current = 0
    pendingStreamLogsRef.current = []
    logAutoScrollRef.current = true
    setStreamLogs([]); setViewNonce(Date.now()); setViewApp(app.name)
  }

  // 程序刚启动时 RTSP 服务可能尚未出首帧，通过更新 nonce 自动重连。
  const STREAM_MAX_RETRY = 25
  const STREAM_STALL_MS  = 4000   // 4 秒未出首帧且未报错时，视为连接卡住。

  // onError 和卡流看门狗共用此重试入口，超过上限后显示错误提示。
  const closeView = () => {
    setViewApp(null)
    setViewControls(null)
    setChannelActionBusy({})
  }

  const handleChannelAction = async (channelId: number, action: LogicActionDef) => {
    if (!viewApp) return
    if (action.confirm && !window.confirm(action.confirm)) return
    const key = `${channelId}:${action.id}`
    setChannelActionBusy(prev => ({ ...prev, [key]: true }))
    try {
      const resp = await sendChannelAction(viewApp, channelId, action.id, action.payload ?? {})
      showToast(resp?.message ? `通道 ${channelId}：${resp.message}` : `通道 ${channelId} 的操作已进入队列`)
    } catch (e) {
      showToast(`通道 ${channelId} 操作失败：${errMsg(e)}`, 'err')
    } finally {
      setChannelActionBusy(prev => ({ ...prev, [key]: false }))
    }
  }

  const scheduleStreamRetry = (delay: number) => {
    if (streamRetryPendingRef.current) return
    if (streamRetryTimer.current) clearTimeout(streamRetryTimer.current)
    if (streamRetryRef.current >= STREAM_MAX_RETRY) { setStreamLoading(false); setStreamErr(true); return }
    streamRetryRef.current += 1
    streamRetryPendingRef.current = true
    setStreamLoading(true)
    streamRetryTimer.current = setTimeout(() => {
      streamRetryPendingRef.current = false
      streamRetryTimer.current = null
      setViewNonce(Date.now())
    }, delay)
  }
  const onStreamLoad  = () => {
    if (streamRetryTimer.current) clearTimeout(streamRetryTimer.current)
    streamRetryTimer.current = null
    streamRetryPendingRef.current = false
    streamRetryRef.current = 0
    setStreamLoading(false)
  }
  const onStreamError = () => scheduleStreamRetry(1500)
  // 用户点击“重试”时重置计数并重新拉流。
  const retryStream = () => {
    streamRetryRef.current = 0
    setStreamErr(false); setStreamLoading(true); setViewNonce(Date.now())
  }

  // Refs for stale-closure-safe access inside setInterval
  const prevRunningRef  = useRef<Set<string>>(new Set())
  const busyRef         = useRef<Record<string, boolean>>({})
  const isFirstLoadRef  = useRef(true)

  // Keep busyRef in sync with busy state
  useEffect(() => { busyRef.current = busy }, [busy])
  // Keep streamLoadingRef in sync so the stall watchdog reads the live value
  useEffect(() => { streamLoadingRef.current = streamLoading }, [streamLoading])

  const load = async () => {
    try {
      const data = await fetchApps()

      // Detect unexpected process exits (skip on very first load)
      if (!isFirstLoadRef.current) {
        for (const app of data) {
          if (
            prevRunningRef.current.has(app.name) &&
            app.status !== 'running' &&
            !busyRef.current[app.name]
          ) {
            // 非用户操作导致进程退出时，读取最后一段日志。
            fetchLogTail(app.name, 40)
              .then(data => setCrashInfo({ name: app.name, lines: Array.isArray(data?.lines) ? data.lines : [] }))
              .catch(() => showToast(`${app.name} 异常退出`, 'err'))
          }
        }
      }
      isFirstLoadRef.current = false

      // Update previous running set
      prevRunningRef.current = new Set(
        data.filter(a => a.status === 'running').map(a => a.name)
      )

      setApps(data)
      const m: Record<string, 'deploy' | 'debug'> = {}
      data.forEach(a => { m[a.name] = (a.mode as 'deploy' | 'debug') ?? 'deploy' })
      setModes(prev => ({ ...m, ...prev }))
    } finally {
      setLoading(false)
    }
  }

  useEffect(() => {
    load()
    const interval = setInterval(load, 5000)
    return () => clearInterval(interval)
  }, []) // eslint-disable-line

  // 实时画面弹窗打开时，通过 WebSocket 在右侧持续显示日志。
  useEffect(() => {
    if (!viewApp) return
    const app = viewApp
    logAutoScrollRef.current = true   // 每次打开弹窗时默认跟随到底部
    pendingStreamLogsRef.current = []
    // 用户查看历史日志时暂停自动滚动。
    const stick = () => { const el = logBoxRef.current; if (el && logAutoScrollRef.current) el.scrollTop = el.scrollHeight }

    fetchLogTail(app, 200)
      .then(d => { setStreamLogs(Array.isArray(d.lines) ? d.lines : []); setTimeout(stick, 50) })
      .catch(() => {})

    const token = useAuthStore.getState().token ?? ''
    const wsUrl = `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws/logs/${encodeURIComponent(app)}?token=${encodeURIComponent(token)}`
    const ws = new WebSocket(wsUrl)
    logWsRef.current = ws
    ws.onmessage = (e) => {
      const text = String(e.data)
      if (!text) return                                   // 蹇冭烦绌哄抚
      const add = text.split('\n').filter(l => l !== '')
      if (add.length) {
        if (logAutoScrollRef.current) {
          setStreamLogs(prev => [...prev, ...add].slice(-1000))
          setTimeout(stick, 10)
        } else {
          // 回看期间不修改当前 DOM，彻底避免头部截断造成的滚动位置跳动。
          // 缓冲区只保存最新 5000 行；回到底部时一次性并入并恢复自动跟随。
          pendingStreamLogsRef.current =
            [...pendingStreamLogsRef.current, ...add].slice(-5000)
        }
      }
    }
    return () => {
      ws.close()
      logWsRef.current = null
      pendingStreamLogsRef.current = []
    }
  }, [viewApp])

  // 关闭弹窗或切换程序时清理重试定时器。
  useEffect(() => () => {
    if (streamRetryTimer.current) { clearTimeout(streamRetryTimer.current); streamRetryTimer.current = null }
    streamRetryPendingRef.current = false
  }, [viewApp])

  // RTSP 已连接但迟迟没有首帧时，img 不一定触发 onLoad/onError。
  // 看门狗会在 STREAM_STALL_MS 后更换连接重新拉流。
  useEffect(() => {
    if (!viewApp || streamErr) return
    const t = setTimeout(() => {
      if (streamLoadingRef.current) scheduleStreamRetry(0)
    }, STREAM_STALL_MS)
    return () => clearTimeout(t)
  }, [viewApp, viewNonce, streamErr]) // eslint-disable-line react-hooks/exhaustive-deps

  // 首帧成功后继续监控后端是否有新视频数据。MJPEG 长连接中途断开时，
  // 浏览器不保证触发 img.onError，必须通过服务端帧心跳识别永久黑屏。
  useEffect(() => {
    if (!viewApp || streamErr) return
    let cancelled = false
    const app = viewApp
    const check = async () => {
      try {
        const health = await fetchStreamHealth(app)
        if (cancelled || streamLoadingRef.current) return
        const stalled = !health.active || health.last_data_age_ms == null || health.last_data_age_ms > 10000
        if (stalled) scheduleStreamRetry(0)
      } catch {
        // 单次健康检查失败不打断当前画面，下一轮继续确认。
      }
    }
    const timer = setInterval(check, 2500)
    return () => { cancelled = true; clearInterval(timer) }
  }, [viewApp, streamErr]) // eslint-disable-line react-hooks/exhaustive-deps

  const dismissToast = () => {
    if (toastTimer.current) { clearTimeout(toastTimer.current); toastTimer.current = null }
    setToast(null)
  }

  const showToast = (msg: string, type: 'ok' | 'err' = 'ok') => {
    if (toastTimer.current) { clearTimeout(toastTimer.current); toastTimer.current = null }
    setToast({ msg, type })
    // 成功提示 3 秒后消失；错误提示保留到用户关闭。
    if (type === 'ok') toastTimer.current = setTimeout(() => { setToast(null); toastTimer.current = null }, 3000)
  }

  const handleStart = async (name: string, config?: string) => {
    setBusy(b => ({ ...b, [name]: true }))
    try {
      const result = await startApp(name, modes[name] ?? 'deploy', config)
      const warnings = result?.service_sync?.errors as string[] | undefined
      if (warnings?.length) {
        showToast(`${name} 已启动，但后台服务同步失败：${warnings.join('；')}`, 'err')
      } else {
        showToast(config && config !== 'config.json' ? `${name} 已使用配置 ${config} 启动` : `${name} 已启动`)
      }
      await load()
    } catch (e: unknown) {
      showToast(`启动失败：${errMsg(e)}`, 'err')
    } finally {
      setBusy(b => ({ ...b, [name]: false }))
    }
  }

  const handleStop = async (name: string) => {
    setBusy(b => ({ ...b, [name]: true }))
    try {
      await stopApp(name)
      showToast(`${name} 已停止`)
      await load()
    } catch (e: unknown) {
      showToast(`停止失败：${errMsg(e)}`, 'err')
    } finally {
      setBusy(b => ({ ...b, [name]: false }))
    }
  }

  const handleUploadFile = async (e: React.ChangeEvent<HTMLInputElement>) => {
    const f = e.target.files?.[0]
    e.target.value = ''                       // 鍏佽鍐嶆閫夊悓涓€鏂囦欢
    if (!f) return
    setUploading({ name: f.name, pct: 0 })
    try {
      const r = await uploadApp(f, undefined, pct => setUploading({ name: f.name, pct }))
      const warn = `${r.has_binary ? '' : '（缺少可执行文件）'}${r.has_config ? '' : '（尚无 config.json）'}`
      const stopped = r.stopped_apps.length > 0
        ? `；已停止视觉程序：${r.stopped_apps.join('、')}`
        : ''
      showToast(`程序 ${r.name} 上传成功${warn}${stopped}`)
      await load()
    } catch (err: unknown) {
      showToast(`上传失败：${errMsg(err)}`, 'err')
    } finally {
      setUploading(null)
    }
  }

  const handleDelete = async (name: string) => {
    if (!window.confirm(`确定删除程序 ${name}？\n此操作会永久删除 /opt/ai_apps/${name}。`)) return
    setBusy(b => ({ ...b, [name]: true }))
    try {
      await deleteApp(name)
      showToast(`程序 ${name} 已删除`)
      await load()
    } catch (e: unknown) {
      showToast(`删除失败：${errMsg(e)}`, 'err')
    } finally {
      setBusy(b => ({ ...b, [name]: false }))
    }
  }

  const handleAutostart = async (name: string, enabled: boolean) => {
    setAutostartBusy(current => ({ ...current, [name]: true }))
    try {
      await setAppAutostart(name, enabled)
      setApps(current => current.map(app => app.name === name ? { ...app, autostart: enabled } : app))
      showToast(`${name} 已${enabled ? '开启' : '关闭'}开机自启`)
    } catch (e: unknown) {
      showToast(`设置开机自启失败：${errMsg(e)}`, 'err')
      await load()
    } finally {
      setAutostartBusy(current => ({ ...current, [name]: false }))
    }
  }

  // 后端负责最终互斥；前端同步禁用其他启动按钮，提前告诉用户需要先停止哪个程序。
  // busy 也纳入判断，覆盖启动请求尚未完成、轮询还没拿到 running 状态的短暂窗口。
  const runningAppName = apps.find(app => app.status === 'running')?.name
  const busyAppName = Object.keys(busy).find(name => busy[name])
  const launchBlockerName = runningAppName ?? busyAppName

  return (
    <div className="apps-page">
      <div className="apps-header">
        <h2>程序管理</h2>
        <div style={{ display: 'flex', gap: 8 }}>
          <button className="reload-btn" disabled={!!uploading}
            onClick={() => fileRef.current?.click()}>上传程序</button>
          <button className="reload-btn" onClick={load}>刷新</button>
        </div>
        <input ref={fileRef} type="file" accept=".zip,.tar.gz,.tgz,.tar"
          style={{ display: 'none' }} onChange={handleUploadFile} />
      </div>

      {toast && (
        <div className={`toast ${toast.type}`}>
          <span className="toast-msg">{toast.msg}</span>
          {toast.type === 'err' && (
            <button className="toast-close" onClick={dismissToast} title="关闭">×</button>
          )}
        </div>
      )}
      {uploading && (
        <div className="toast ok">正在上传 {uploading.name} — {uploading.pct}%</div>
      )}

      {/* Crash log dialog */}
      {crashInfo && (
        <div className="crash-overlay" onClick={() => setCrashInfo(null)}>
          <div className="crash-dialog" onClick={e => e.stopPropagation()}>
            <div className="crash-header">
              <span>程序异常退出：{crashInfo.name}</span>
              <button onClick={() => setCrashInfo(null)}>×</button>
            </div>
            <div className="crash-subtext">
              异常退出前的最后日志：
            </div>
            <pre className="crash-log">
              {crashInfo.lines.length > 0
                ? crashInfo.lines.join('\n')
                : '（日志为空）'}
            </pre>
            <div className="crash-footer">
              <button
                className="crash-log-btn"
                onClick={() => {
                  const name = crashInfo.name
                  setCrashInfo(null)
                  navigate(`/logs/${name}`)
                }}
              >
                查看完整日志
              </button>
              <button className="crash-close-btn" onClick={() => setCrashInfo(null)}>
                关闭
              </button>
            </div>
          </div>
        </div>
      )}

      {/* Live stream dialog */}
      {viewApp && (
        <div className="stream-overlay">
          <div className="stream-dialog">
            <div className="stream-header">
              <span>{viewApp} — 实时画面</span>
              <button onClick={closeView}>×</button>
            </div>
            <div className="stream-body">
              <div className="stream-video-column">
              <div className="stream-video">
                {!streamErr ? (
                  <>
                    <img
                      className="stream-img"
                      style={streamLoading ? { visibility: 'hidden' } : undefined}
                      src={`${streamUrl(viewApp)}&t=${viewNonce}`}
                      alt="实时画面"
                      onLoad={onStreamLoad}
                      onError={onStreamError}
                    />
                    {streamLoading && (
                      <div className="stream-loading">
                        <div className="stream-spinner" />
                        <span>正在加载视频……</span>
                      </div>
                    )}
                  </>
                ) : (
                  <div className="stream-hint">
                    实时视频暂不可用。<br />
                    1. 请确认程序正在运行。<br />
                    2. 请在全局配置中启用 <b>RTSP 推流</b>，然后重启程序。
                    <br />
                    <button className="stream-retry-btn" onClick={retryStream}>重试</button>
                  </div>
                )}
              </div>
              <div className="stream-channel-controls">
                <div className="stream-channel-controls-head">
                  <span>通道控制</span>
                  {!viewControls?.socket_ready && <span className="stream-control-badge">未连接</span>}
                </div>
                {!viewControls ? (
                  <div className="stream-channel-empty">暂时无法获取通道控制信息。</div>
                ) : viewControls.channels.length === 0 ? (
                  <div className="stream-channel-empty">当前配置中没有通道。</div>
                ) : (
                  <div className="stream-channel-grid">
                    {viewControls.channels.map(channel => (
                      <div key={channel.channel_id} className="stream-channel-card">
                        <div className="stream-channel-title">
                          <span>{`通道 ${channel.channel_id}`}</span>
                          <span className="stream-channel-logic">{channel.logic_label}</span>
                        </div>
                        <div className="stream-channel-actions">
                          {channel.actions.map(action => {
                            const key = `${channel.channel_id}:${action.id}`
                            const disabled = !viewControls.socket_ready || !channel.enabled || !!channelActionBusy[key]
                            return (
                              <button
                                key={key}
                                className={`stream-action-btn ${action.style ?? 'default'}`}
                                disabled={disabled}
                                title={action.help ?? action.id}
                                onClick={() => handleChannelAction(channel.channel_id, action)}
                              >
                                {channelActionBusy[key] ? '...' : (action.label ?? action.id)}
                              </button>
                            )
                          })}
                        </div>
                      </div>
                    ))}
                  </div>
                )}
              </div>
              </div>
              <div className="stream-logs" ref={logBoxRef} onScroll={onStreamLogScroll}>
                {streamLogs.length === 0
                  ? <div className="stream-logs-empty">暂无日志。</div>
                  : streamLogs.map((line, i) => (
                      <div key={i} className={`stream-log-line${/ERROR|error|\[进程已停止\]/.test(line) ? ' err' : /WARN/.test(line) ? ' warn' : ''}`}>
                        {line}
                      </div>
                    ))}
              </div>
            </div>
            <div className="stream-footer">
              <span className="stream-tip">按钮操作按通道进入队列，并在下一帧 C++ 通道逻辑执行前处理。</span>
            </div>
          </div>
        </div>
      )}

      {loading ? (
        <div className="loading">正在加载……</div>
      ) : apps.length === 0 ? (
        <div className="empty">程序目录中暂无可用程序。</div>
      ) : (
        <div className="app-grid">
          {apps.map(app => {
            // 程序可选的启动配置文件名，以及当前选中的配置。
            const cfgOpts = app.config_files.map(cfgName)
            // 优先级：手动选择 > 编辑器最后保存 > 上次启动 > config.json > 第一项。
            const lastCfg = loadLastConfig(app.name)
            const effCfg  = cfgSel[app.name]
              ?? (lastCfg && cfgOpts.includes(lastCfg) ? lastCfg
                  : cfgOpts.includes(app.active_config) ? app.active_config
                  : cfgOpts.includes('config.json')     ? 'config.json'
                  : cfgOpts[0] ?? 'config.json')
            const blockedByOtherApp = !!launchBlockerName && launchBlockerName !== app.name
            return (
            <div key={app.name} className={`app-card ${app.status}`}>
              <div className="card-top">
                <div className="app-name">{app.name}</div>
                <span className={`status-badge ${app.status}`}>
                  {app.status === 'running' ? '运行中' : '已停止'}
                </span>
              </div>

              <div className="card-meta">
                {app.status === 'running' && (
                  <>
                    <span>PID: {app.pid}</span>
                    <span>运行时间：{fmtUptime(app.uptime_seconds)}</span>
                    <span>模式：{app.mode === 'debug' ? '调试' : '部署'}</span>
                    {app.config && <span>配置：{app.config}</span>}
                  </>
                )}
                <span>模型数量：{app.models.length}</span>
                {!app.has_binary && <span className="warn">缺少可执行文件</span>}
                {app.config_files.length === 0 && <span className="warn">缺少配置文件</span>}
                {app.status !== 'running' && blockedByOtherApp && (
                  <span className="warn">请先停止程序 {launchBlockerName}</span>
                )}
              </div>

              {/* 始终显示启动配置；程序运行时禁用修改，保持卡片布局稳定。 */}
              {cfgOpts.length >= 1 && (
                <div className="config-row">
                  <label>启动配置</label>
                  <select
                    value={app.status === 'running'
                      ? (cfgOpts.includes(app.config ?? '') ? (app.config as string) : effCfg)
                      : effCfg}
                    disabled={app.status === 'running'}
                    onChange={e => setCfgSel(s => ({ ...s, [app.name]: e.target.value }))}
                  >
                    {cfgOpts.map(o => <option key={o} value={o}>{o}</option>)}
                  </select>
                </div>
              )}

              <div className="card-actions">
                <div className="mode-toggle">
                  <button
                    className={`mode-btn${(modes[app.name] ?? 'deploy') === 'deploy' ? ' active' : ''}`}
                    onClick={() => setModes(m => ({ ...m, [app.name]: 'deploy' }))}
                    disabled={app.status === 'running'}
                  >部署</button>
                  <button
                    className={`mode-btn${(modes[app.name] ?? 'deploy') === 'debug' ? ' active' : ''}`}
                    onClick={() => setModes(m => ({ ...m, [app.name]: 'debug' }))}
                    disabled={app.status === 'running'}
                  >调试</button>
                </div>

                <div className="action-btns">
                  <label className="autostart-toggle" title="勾选后，仅当关机前最后状态为运行时才会在下次开机恢复">
                    <input
                      type="checkbox"
                      checked={app.autostart}
                      disabled={!!autostartBusy[app.name] || !!busy[app.name]}
                      onChange={e => handleAutostart(app.name, e.target.checked)}
                    />
                    <span>{autostartBusy[app.name] ? '保存中' : '开机自启'}</span>
                  </label>
                  {app.status !== 'running' ? (
                    <button
                      className="action-btn start"
                      disabled={!app.has_binary || app.config_files.length === 0 || !!busy[app.name] || blockedByOtherApp}
                      title={blockedByOtherApp ? `同一时间只能运行一个程序，请先停止 ${launchBlockerName}` : '启动程序'}
                      onClick={() => handleStart(app.name, effCfg)}
                    >
                      {busy[app.name] ? '...' : '启动'}
                    </button>
                  ) : (
                    <button
                      className="action-btn stop"
                      disabled={!!busy[app.name]}
                      onClick={() => handleStop(app.name)}
                    >
                      {busy[app.name] ? '...' : '停止'}
                    </button>
                  )}

                  <button
                    className="action-btn view"
                    disabled={app.status !== 'running'}
                    title={app.status === 'running' ? '查看实时画面' : '仅在程序运行时可用'}
                    onClick={() => openView(app)}
                  >实时画面</button>

                  <button
                    className="action-btn edit"
                    onClick={() => navigate(`/editor/${app.name}?config=${encodeURIComponent(effCfg)}`)}
                  >配置</button>

                  <button
                    className="action-btn log"
                    onClick={() => navigate(`/logs/${app.name}`)}
                  >日志</button>

                  <button
                    className="action-btn log"
                    onClick={() => navigate(`/records/${app.name}`)}
                    title="查看本地发件箱中的待上报记录"
                  >待上报记录（{app.unreported ?? 0}条）</button>

                  <button
                    className="action-btn"
                    style={{ background: '#7f1d1d', color: '#fff' }}
                    disabled={!!busy[app.name]}
                    onClick={() => handleDelete(app.name)}
                    title="停止并删除该程序包"
                  >删除</button>
                </div>
              </div>
            </div>
            )
          })}
        </div>
      )}
    </div>
  )
}
