import { useEffect, useRef, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import axios from 'axios'
import { fetchApps, fetchLogTail, startApp, stopApp, streamUrl, uploadApp, deleteApp, AppInfo } from '../api/client'
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
  const [streamLogs, setStreamLogs] = useState<string[]>([])          // 监看弹窗右侧的滚动日志
  const [viewNonce, setViewNonce]   = useState(0)                     // 每次打开换一个值, 强制刷新视频, 防残留上次的旧帧
  const logWsRef  = useRef<WebSocket | null>(null)
  const logBoxRef = useRef<HTMLDivElement>(null)
  const fileRef   = useRef<HTMLInputElement>(null)
  const [uploading, setUploading] = useState<{ name: string; pct: number } | null>(null)
  const navigate = useNavigate()

  const openView = (name: string) => { setStreamErr(false); setStreamLogs([]); setViewNonce(Date.now()); setViewApp(name) }

  // Refs for stale-closure-safe access inside setInterval
  const prevRunningRef  = useRef<Set<string>>(new Set())
  const busyRef         = useRef<Record<string, boolean>>({})
  const isFirstLoadRef  = useRef(true)

  // Keep busyRef in sync with busy state
  useEffect(() => { busyRef.current = busy }, [busy])

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
    const stick = () => { const el = logBoxRef.current; if (el) el.scrollTop = el.scrollHeight }

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
      if (add.length) { setStreamLogs(prev => [...prev.slice(-1000), ...add]); setTimeout(stick, 10) }
    }
    return () => { ws.close(); logWsRef.current = null }
  }, [viewApp])

  const showToast = (msg: string, type: 'ok' | 'err' = 'ok') => {
    setToast({ msg, type })
    setTimeout(() => setToast(null), 3000)
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

      {toast && <div className={`toast ${toast.type}`}>{toast.msg}</div>}
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
        <div className="stream-overlay" onClick={() => setViewApp(null)}>
          <div className="stream-dialog" onClick={e => e.stopPropagation()}>
            <div className="stream-header">
              <span>👁 {viewApp} · 实时画面</span>
              <button onClick={() => setViewApp(null)}>✕</button>
            </div>
            <div className="stream-body">
              <div className="stream-video">
                {!streamErr ? (
                  <img
                    className="stream-img"
                    src={`${streamUrl(viewApp)}&t=${viewNonce}`}
                    alt="实时画面"
                    onError={() => setStreamErr(true)}
                  />
                ) : (
                  <div className="stream-hint">
                    无法获取视频流。请确认：<br />
                    ① 程序正在运行（监看仅在运行时可用）；<br />
                    ② 已在「配置 → 全局配置」勾选 <b>RTSP 推流</b> 并重启程序。
                  </div>
                )}
              </div>
              <div className="stream-logs" ref={logBoxRef}>
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
            const effCfg  = cfgSel[app.name]
              ?? (cfgOpts.includes(app.active_config) ? app.active_config
                  : cfgOpts.includes('config.json')   ? 'config.json'
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

              {/* 启动配置选择：存在多份配置文件时显示；运行中变灰不可改（保持卡片布局不跳动） */}
              {cfgOpts.length > 1 && (
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
                    onClick={() => openView(app.name)}
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
