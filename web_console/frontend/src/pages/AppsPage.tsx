import { useEffect, useRef, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import axios from 'axios'
import { fetchApps, fetchLogTail, startApp, stopApp, setAppAutostart, uploadApp, deleteApp, AppInfo } from '../api/client'
import { loadLastConfig } from '../utils/lastConfig'
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
  const fileRef   = useRef<HTMLInputElement>(null)
  const toastTimer = useRef<ReturnType<typeof setTimeout> | null>(null)
  const [uploading, setUploading] = useState<{ name: string; pct: number } | null>(null)
  const navigate = useNavigate()

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
    if (!window.confirm(`确定删除程序 ${name}？\n此操作会永久删除该程序目录。`)) return
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
