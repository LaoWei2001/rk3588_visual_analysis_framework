import { useEffect, useRef, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import axios from 'axios'
import { fetchApps, fetchLogTail, startApp, stopApp, streamUrl, uploadApp, deleteApp, fetchConfig, loadConfigFile, AppInfo } from '../api/client'
import { loadLastConfig } from '../utils/lastConfig'
import { useAuthStore } from '../store/authStore'
import ServicesPanel from '../components/ServicesPanel'
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

// 'assets/config.json' → 'config.json'（下拉显示与 active_config/运行态 config 对齐）
const cfgName = (p: string): string => p.split('/').pop() ?? p

export default function AppsPage() {
  const [apps, setApps]       = useState<AppInfo[]>([])
  const [loading, setLoading] = useState(true)
  const [modes, setModes]     = useState<Record<string, 'deploy' | 'debug'>>({})
  const [cfgSel, setCfgSel]   = useState<Record<string, string>>({})   // 每个程序选中的启动配置文件名
  const [busy, setBusy]       = useState<Record<string, boolean>>({})
  const [toast, setToast]     = useState<{ msg: string; type: 'ok' | 'err' } | null>(null)
  const [crashInfo, setCrashInfo] = useState<{ name: string; lines: string[] } | null>(null)
  const [viewApp, setViewApp]       = useState<string | null>(null)   // 正在监看的 App
  const [streamErr, setStreamErr]   = useState(false)
  const [streamLoading, setStreamLoading] = useState(true)           // 视频首帧到达前显示加载动画
  const [streamLogs, setStreamLogs] = useState<string[]>([])          // 监看弹窗右侧的滚动日志
  const [viewNonce, setViewNonce]   = useState(0)                     // 每次打开换一个值, 强制刷新视频, 防残留上次的旧帧
  const streamRetryRef   = useRef(0)                                  // 流未就绪(刚启动)时的自动重试计数
  const streamRetryTimer = useRef<ReturnType<typeof setTimeout> | null>(null)
  const streamLoadingRef = useRef(true)                               // 供卡死看门狗读「当前是否仍在加载」(避免闭包取旧值)
  const logWsRef  = useRef<WebSocket | null>(null)
  const logBoxRef = useRef<HTMLDivElement>(null)
  // 监看日志是否“跟随到底”：在底部(40px 内)=跟随，往上拉=暂停并停在当前位置，拉回底部=自动恢复
  const logAutoScrollRef = useRef(true)
  const onStreamLogScroll = () => {
    const el = logBoxRef.current
    if (el) logAutoScrollRef.current = el.scrollHeight - el.scrollTop - el.clientHeight < 40
  }
  const fileRef   = useRef<HTMLInputElement>(null)
  const toastTimer = useRef<ReturnType<typeof setTimeout> | null>(null)
  const [uploading, setUploading] = useState<{ name: string; pct: number } | null>(null)
  const navigate = useNavigate()

  // 打开实时画面前先检查该程序是否开启了 RTSP 推流——没开则必然黑屏，提前提示而不是让用户等失败。
  const openView = async (app: AppInfo) => {
    try {
      const running = app.config && app.config !== 'config.json' ? `assets/${app.config}` : null
      const cfg = running ? await loadConfigFile(app.name, running) : await fetchConfig(app.name)
      const g = (cfg && ((cfg as Record<string, unknown>).global ?? cfg)) as Record<string, unknown> | null
      // 读到配置且明确未开启才拦截；读不到（异常/旧配置）一律放行，避免误拦正常推流
      if (g && !Number(g.enable_rtsp ?? 0)) {
        showToast(`${app.name} 未开启 RTSP 推流，无法显示实时画面。请到「配置 → 全局配置」勾选「RTSP 推流」并重启程序。`, 'err')
        return
      }
    } catch { /* 配置读取失败：不拦截，照常打开（仍会回退到弹窗内的原有提示） */ }
    setStreamErr(false); setStreamLoading(true); streamRetryRef.current = 0
    setStreamLogs([]); setViewNonce(Date.now()); setViewApp(app.name)
  }

  // 视频流自动重试: 程序刚启动时 RTSP 服务/首帧还没就绪, 直接拉流会失败或卡住。
  // 不再直接判失败黑屏, 而是换 nonce 重新拉流, 直到出帧(onLoad)或超过重试上限,
  // 这样画面会自己加载出来, 不用退出再进入。
  const STREAM_MAX_RETRY = 25     // 重试上限(到顶才显示错误提示)
  const STREAM_STALL_MS  = 4000   // 4s 内既没出首帧也没报错 = 卡住(常见于"刚启动就极快点进"), 换条连接重连

  // 安排下一次重连: onError(连接被拒, 隔 1.5s) 与卡死看门狗(立即) 共用; 超过上限则放弃并提示
  const scheduleStreamRetry = (delay: number) => {
    if (streamRetryTimer.current) clearTimeout(streamRetryTimer.current)
    if (streamRetryRef.current >= STREAM_MAX_RETRY) { setStreamLoading(false); setStreamErr(true); return }
    streamRetryRef.current += 1
    setStreamLoading(true)
    streamRetryTimer.current = setTimeout(() => setViewNonce(Date.now()), delay)
  }
  const onStreamLoad  = () => { streamRetryRef.current = 0; setStreamLoading(false) }
  const onStreamError = () => scheduleStreamRetry(1500)
  // 错误提示里的「重试」: 重置计数并重新开始拉流
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
            // Process died without user action — fetch last log lines
            fetchLogTail(app.name, 40)
              .then(data => setCrashInfo({ name: app.name, lines: Array.isArray(data?.lines) ? data.lines : [] }))
              .catch(() => showToast(`${app.name} 进程意外退出`, 'err'))
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

  // 监看弹窗打开时：连日志 WebSocket，实时滚动显示在视频右侧
  useEffect(() => {
    if (!viewApp) return
    const app = viewApp
    logAutoScrollRef.current = true   // 每次打开监看默认跟随到底
    // 只有用户停在底部时才跟随；拉上去看历史就停住，不再被新日志拽回底部
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
      if (!text) return                                   // 心跳空帧
      const add = text.split('\n').filter(l => l !== '')
      if (add.length) {
        // 跟随到底时维持 1000 行上限；用户拉上去看历史时不裁顶部(放宽到 5000)——
        // 否则每来一批日志就从顶部裁掉旧行，会把视口里的历史内容往上挤 → 闪烁、看不清。
        // 滚回底部恢复跟随后，裁剪发生在视口上方且贴底锁定，看不到跳动。
        setStreamLogs(prev => {
          const next = [...prev, ...add]
          return logAutoScrollRef.current ? next.slice(-1000) : next.slice(-5000)
        })
        setTimeout(stick, 10)
      }
    }
    return () => { ws.close(); logWsRef.current = null }
  }, [viewApp])

  // 关弹窗/切换 App 时清掉待执行的重试定时器, 避免泄漏或对已关闭的弹窗刷流
  useEffect(() => () => {
    if (streamRetryTimer.current) { clearTimeout(streamRetryTimer.current); streamRetryTimer.current = null }
  }, [viewApp])

  // 卡死看门狗: 后端可能连上了 RTSP 但迟迟不出首帧, <img> 既不触发 onLoad 也不触发 onError,
  // 画面就会一直转圈("程序刚启动就极快点进"最容易撞上)。每次发起拉流(viewNonce 变)后等
  // STREAM_STALL_MS, 若仍在加载就换条连接重连 —— 后端单飞机制会顺带杀掉那条卡住的旧流。
  useEffect(() => {
    if (!viewApp || streamErr) return
    const t = setTimeout(() => {
      if (streamLoadingRef.current) scheduleStreamRetry(0)
    }, STREAM_STALL_MS)
    return () => clearTimeout(t)
  }, [viewApp, viewNonce, streamErr]) // eslint-disable-line react-hooks/exhaustive-deps

  const dismissToast = () => {
    if (toastTimer.current) { clearTimeout(toastTimer.current); toastTimer.current = null }
    setToast(null)
  }

  const showToast = (msg: string, type: 'ok' | 'err' = 'ok') => {
    if (toastTimer.current) { clearTimeout(toastTimer.current); toastTimer.current = null }
    setToast({ msg, type })
    // 成功提示 3s 自动消失；错误提示常驻，直到用户点 ✕ 关闭，避免一扭头就错过失败原因
    if (type === 'ok') toastTimer.current = setTimeout(() => { setToast(null); toastTimer.current = null }, 3000)
  }

  const handleStart = async (name: string, config?: string) => {
    setBusy(b => ({ ...b, [name]: true }))
    try {
      await startApp(name, modes[name] ?? 'deploy', config)
      showToast(`${name} 已启动${config && config !== 'config.json' ? `（配置: ${config}）` : ''}`)
      await load()
    } catch (e: unknown) {
      showToast(`启动失败: ${errMsg(e)}`, 'err')
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
      showToast(`停止失败: ${errMsg(e)}`, 'err')
    } finally {
      setBusy(b => ({ ...b, [name]: false }))
    }
  }

  const handleUploadFile = async (e: React.ChangeEvent<HTMLInputElement>) => {
    const f = e.target.files?.[0]
    e.target.value = ''                       // 允许再次选同一文件
    if (!f) return
    setUploading({ name: f.name, pct: 0 })
    try {
      const r = await uploadApp(f, undefined, pct => setUploading({ name: f.name, pct }))
      const warn = `${r.has_binary ? '' : '（⚠ 无二进制）'}${r.has_config ? '' : '（⚠ 无 config.json，需在编辑器存一份）'}`
      showToast(`已上传程序: ${r.name} ${warn}`)
      await load()
    } catch (err: unknown) {
      showToast(`上传失败: ${errMsg(err)}`, 'err')
    } finally {
      setUploading(null)
    }
  }

  const handleDelete = async (name: string) => {
    if (!window.confirm(`确定删除程序「${name}」？\n会先停止其进程，并删除 /opt/ai_apps/${name} 整个目录，不可恢复。`)) return
    setBusy(b => ({ ...b, [name]: true }))
    try {
      await deleteApp(name)
      showToast(`已删除: ${name}`)
      await load()
    } catch (e: unknown) {
      showToast(`删除失败: ${errMsg(e)}`, 'err')
    } finally {
      setBusy(b => ({ ...b, [name]: false }))
    }
  }

  return (
    <div className="apps-page">
      <div className="apps-header">
        <h2>程序管理</h2>
        <div style={{ display: 'flex', gap: 8 }}>
          <button className="reload-btn" disabled={!!uploading}
            onClick={() => fileRef.current?.click()}>⬆ 上传程序包</button>
          <button className="reload-btn" onClick={load}>↻ 刷新</button>
        </div>
        <input ref={fileRef} type="file" accept=".zip,.tar.gz,.tgz,.tar"
          style={{ display: 'none' }} onChange={handleUploadFile} />
      </div>

      {toast && (
        <div className={`toast ${toast.type}`}>
          <span className="toast-msg">{toast.msg}</span>
          {toast.type === 'err' && (
            <button className="toast-close" onClick={dismissToast} title="关闭">✕</button>
          )}
        </div>
      )}
      {uploading && (
        <div className="toast ok">⬆ 上传中 {uploading.name} … {uploading.pct}%</div>
      )}

      <ServicesPanel apps={apps} onToast={showToast} />

      {/* Crash log dialog */}
      {crashInfo && (
        <div className="crash-overlay" onClick={() => setCrashInfo(null)}>
          <div className="crash-dialog" onClick={e => e.stopPropagation()}>
            <div className="crash-header">
              <span>⚠ {crashInfo.name} 进程意外退出</span>
              <button onClick={() => setCrashInfo(null)}>✕</button>
            </div>
            <div className="crash-subtext">
              以下为 run.log 末尾输出，可帮助定位原因：
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
              <span>👁 {viewApp} · 实时画面</span>
              <button onClick={() => setViewApp(null)}>✕</button>
            </div>
            <div className="stream-body">
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
                        <span>正在加载视频…</span>
                      </div>
                    )}
                  </>
                ) : (
                  <div className="stream-hint">
                    无法获取视频流。请确认：<br />
                    ① 程序正在运行（监看仅在运行时可用）；<br />
                    ② 已在「配置 → 全局配置」勾选 <b>RTSP 推流</b> 并重启程序。
                    <br />
                    <button className="stream-retry-btn" onClick={retryStream}>重试</button>
                  </div>
                )}
              </div>
              <div className="stream-logs" ref={logBoxRef} onScroll={onStreamLogScroll}>
                {streamLogs.length === 0
                  ? <div className="stream-logs-empty">暂无日志…</div>
                  : streamLogs.map((line, i) => (
                      <div key={i} className={`stream-log-line${/ERROR|error|\[进程已停止\]/.test(line) ? ' err' : /WARN/.test(line) ? ' warn' : ''}`}>
                        {line}
                      </div>
                    ))}
              </div>
            </div>
            <div className="stream-footer">
              <span className="stream-tip">画面与接显示器一致（含检测框/叠加）。点右上角 ✕ 关闭弹窗即停止拉流。</span>
            </div>
          </div>
        </div>
      )}

      {loading ? (
        <div className="loading">加载中…</div>
      ) : apps.length === 0 ? (
        <div className="empty">未找到任何算法程序。请检查 APPS_ROOT 目录。</div>
      ) : (
        <div className="app-grid">
          {apps.map(app => {
            // 该程序可选的启动配置文件（basename），以及当前选中的那个
            const cfgOpts = app.config_files.map(cfgName)
            // 优先级：本次手动选择 > 编辑器里最后保存的配置 > 上次启动配置 > config.json > 第一个
            const lastCfg = loadLastConfig(app.name)
            const effCfg  = cfgSel[app.name]
              ?? (lastCfg && cfgOpts.includes(lastCfg) ? lastCfg
                  : cfgOpts.includes(app.active_config) ? app.active_config
                  : cfgOpts.includes('config.json')     ? 'config.json'
                  : cfgOpts[0] ?? 'config.json')
            return (
            <div key={app.name} className={`app-card ${app.status}`}>
              <div className="card-top">
                <div className="app-name">{app.name}</div>
                <span className={`status-badge ${app.status}`}>
                  {app.status === 'running' ? '● 运行中' : '○ 已停止'}
                </span>
              </div>

              <div className="card-meta">
                {app.status === 'running' && (
                  <>
                    <span>PID: {app.pid}</span>
                    <span>运行: {fmtUptime(app.uptime_seconds)}</span>
                    <span>模式: {app.mode === 'debug' ? '调试' : '部署'}</span>
                    {app.config && <span>配置: {app.config}</span>}
                  </>
                )}
                <span>模型: {app.models.length} 个</span>
                {!app.has_binary && <span className="warn">⚠ 无可执行文件</span>}
                {app.config_files.length === 0 && <span className="warn">⚠ 无配置文件</span>}
              </div>

              {/* 启动配置选择：只要有配置文件就显示（含仅 1 份的情况）；运行中变灰不可改（保持卡片布局不跳动） */}
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
                  {app.status !== 'running' ? (
                    <button
                      className="action-btn start"
                      disabled={!app.has_binary || app.config_files.length === 0 || !!busy[app.name]}
                      onClick={() => handleStart(app.name, effCfg)}
                    >
                      {busy[app.name] ? '…' : '▶ 启动'}
                    </button>
                  ) : (
                    <button
                      className="action-btn stop"
                      disabled={!!busy[app.name]}
                      onClick={() => handleStop(app.name)}
                    >
                      {busy[app.name] ? '…' : '■ 停止'}
                    </button>
                  )}

                  <button
                    className="action-btn view"
                    disabled={app.status !== 'running'}
                    title={app.status === 'running' ? '查看实时画面' : '程序运行后才能查看'}
                    onClick={() => openView(app)}
                  >👁 实时画面</button>

                  <button
                    className="action-btn edit"
                    onClick={() => navigate(`/editor/${app.name}?config=${encodeURIComponent(effCfg)}`)}
                  >⚙ 配置</button>

                  <button
                    className="action-btn log"
                    onClick={() => navigate(`/logs/${app.name}`)}
                  >≡ 日志</button>

                  <button
                    className="action-btn log"
                    onClick={() => navigate(`/records/${app.name}`)}
                    title="本地暂存、还没传到平台的告警(断网时攒在盒子里的)"
                  >🖼 未上报告警{(app.unreported ?? 0) > 0 ? `（${app.unreported}条）` : ''}</button>

                  <button
                    className="action-btn"
                    style={{ background: '#7f1d1d', color: '#fff' }}
                    disabled={!!busy[app.name]}
                    onClick={() => handleDelete(app.name)}
                    title="停止并删除该程序包（不可恢复）"
                  >🗑 删除</button>
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
