/**
 * ServicesPanel — 板级「后台服务」面板，由左侧“服务配置”独立页面承载。
 *
 * 托管两个 systemd 单元(OTA 升级 / 告警上报): 安装/启停/重启/看健康/看日志。
 * systemd 是唯一进程管家, 与命令行 deploy.sh / systemctl 操作同一套单元, 并存不冲突。
 */
import { useEffect, useRef, useState } from 'react'
import axios from 'axios'
import {
  fetchServices, controlService, setServiceAutostart, fetchServiceLogs,
  type ServiceInfo, type AppInfo,
} from '../api/client'

interface Props {
  apps: AppInfo[]
  onToast: (msg: string, type?: 'ok' | 'err') => void
}

function errMsg(e: unknown): string {
  if (axios.isAxiosError(e)) return e.response?.data?.detail ?? e.message
  return e instanceof Error ? e.message : String(e)
}

function fmtUptime(s: number | null): string {
  if (s == null) return '-'
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60)
  if (h > 0) return `${h}h ${m}m`
  if (m > 0) return `${m}m`
  return `${s}s`
}

function statusBadge(s: ServiceInfo): { text: string; color: string } {
  if (!s.installed) return { text: '未安装', color: '#f59e0b' }
  if (!s.path_ok)   return { text: '⚠ 路径失效', color: '#f59e0b' }
  switch (s.active_state) {
    case 'active':     return { text: '● 运行中', color: '#22c55e' }
    case 'failed':     return { text: '✕ 故障',   color: '#ef4444' }
    case 'activating': return { text: '… 启动中', color: '#3b82f6' }
    default:           return { text: '○ 已停止', color: '#9aa4b2' }
  }
}

const panel: React.CSSProperties = {
  background: '#1a1d29', border: '1px solid #2e3352', borderRadius: 10,
  padding: '12px 16px', marginBottom: 18,
}
const row: React.CSSProperties = {
  display: 'flex', alignItems: 'center', gap: 12, padding: '10px 0',
  borderTop: '1px solid #23263a', flexWrap: 'wrap',
}
const btn = (bg: string): React.CSSProperties => ({
  background: bg, color: '#fff', border: 'none', borderRadius: 6,
  padding: '5px 12px', cursor: 'pointer', fontSize: 13,
})
const ghost: React.CSSProperties = {
  background: '#2e3352', color: '#e6e9ef', border: 'none', borderRadius: 6,
  padding: '5px 12px', cursor: 'pointer', fontSize: 13,
}
const disabledStartBtn: React.CSSProperties = {
  ...btn('#4b5563'), color: '#cbd5e1', cursor: 'not-allowed', opacity: 0.75,
}
export default function ServicesPanel({ apps, onToast }: Props) {
  const [services, setServices] = useState<ServiceInfo[]>([])
  const [busy, setBusy] = useState<Record<string, boolean>>({})
  const [autostartBusy, setAutostartBusy] = useState<Record<string, boolean>>({})
  const [logKey, setLogKey] = useState<string | null>(null)
  const [logLines, setLogLines] = useState<string[]>([])
  const [loaded, setLoaded] = useState(false)
  const logContainerRef = useRef<HTMLPreElement>(null)
  const autoScrollRef = useRef(true)
  const clearedMarkersRef = useRef<Record<string, string>>({})  // 清空游标（按服务 key 持久化）

  const scrollToBottom = () => {
    const el = logContainerRef.current
    if (el && autoScrollRef.current) el.scrollTop = el.scrollHeight
  }

  const handleLogScroll = () => {
    const el = logContainerRef.current
    if (!el) return
    const atBottom = el.scrollHeight - el.scrollTop - el.clientHeight < 40
    if (!atBottom) autoScrollRef.current = false
    else autoScrollRef.current = true
  }

  const load = async () => {
    try { setServices(await fetchServices()) } catch { /* 板端无 systemctl 时忽略 */ }
    finally { setLoaded(true) }
  }
  useEffect(() => {
    load()
    const t = setInterval(load, 5000)
    return () => clearInterval(t)
  }, [])

  // 日志弹窗打开期间定时拉取新日志，模拟实时滚动
  useEffect(() => {
    if (!logKey) return
    const fetchLogs = async () => {
      try {
        const d = await fetchServiceLogs(logKey, 300)
        let lines = d.lines.length ? d.lines : ['（暂无日志）']
        // 如果该服务有清空游标，只取游标之后的新行
        const marker = clearedMarkersRef.current[logKey]
        if (marker) {
          const idx = lines.indexOf(marker)
          lines = idx >= 0 ? lines.slice(idx + 1) : lines
        }
        setLogLines(lines)
        setTimeout(scrollToBottom, 10)
      } catch (e) {
        setLogLines(['[日志获取失败] ' + errMsg(e)])
      }
    }
    fetchLogs()
    const t = setInterval(fetchLogs, 3000)
    return () => clearInterval(t)
  }, [logKey])

  const act = async (key: string, fn: () => Promise<unknown>, okMsg: string) => {
    setBusy(b => ({ ...b, [key]: true }))
    try { await fn(); onToast(okMsg); await load() }
    catch (e) { onToast(errMsg(e), 'err') }
    finally { setBusy(b => ({ ...b, [key]: false })) }
  }

  const handleClearLogs = () => {
    // 记录当前最后一行作为游标（按服务 key 持久化），后续轮询只取此标记之后的新行
    if (logLines.length > 0 && logKey) clearedMarkersRef.current[logKey] = logLines[logLines.length - 1]
    setLogLines([])
  }

  const handleAutostart = async (key: string, enabled: boolean) => {
    setAutostartBusy(current => ({ ...current, [key]: true }))
    try {
      await setServiceAutostart(key, enabled)
      setServices(current => current.map(service =>
        service.key === key ? { ...service, autostart: enabled } : service
      ))
      onToast(`${services.find(service => service.key === key)?.label ?? key} 已${enabled ? '开启' : '关闭'}开机自启`)
    } catch (e) {
      onToast(`设置开机自启失败：${errMsg(e)}`, 'err')
      await load()
    } finally {
      setAutostartBusy(current => ({ ...current, [key]: false }))
    }
  }

  if (!loaded) return null

  return (
    <div style={panel}>
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
        <span style={{ fontWeight: 600, fontSize: 15 }}>⚙ 后台服务</span>
        <span style={{ fontSize: 12, color: '#9aa4b2' }}>自动跟随当前运行的视觉程序</span>
      </div>

      {services.map(s => {
        const b = statusBadge(s)
        const isBusy = !!busy[s.key]
        const runningApp = apps.find(app => app.status === 'running')
        // "开着"判定：运行中 / 启动中 / 故障(反复尝试重启) 都算开 → 显示「停止」，
        // 方便在服务一直起不来、反复重启时摁停下来排查。只有干净 inactive / 未知才显示「启动」。
        const started = s.active_state !== 'inactive' && s.active_state !== 'unknown'
        // 需要走安装/修复：没装，或装了但单元工作目录失效（路径不存在/指向已删的旧目录）
        const needsInstall = !s.installed || !s.path_ok
        return (
          <div key={s.key} style={row}>
            <div style={{ flex: 1, minWidth: 0 }}>
              <div style={{ fontWeight: 600 }}>{s.label}</div>
              <div style={{ fontSize: 12, color: '#9aa4b2', marginTop: 2 }}>
                {s.unit}
                {s.installed && s.path_ok && s.bound_app && ` · 绑定 ${s.bound_app}`}
                {s.key === 'ota_agent' && s.bound_config && ` / ${s.bound_config}`}
                {s.installed && !s.path_ok && s.working_dir && ` · ⚠ 旧路径不存在：${s.working_dir}`}
                {s.active_state === 'active' && s.path_ok && ` · 运行 ${fmtUptime(s.uptime_seconds)}`}
                {s.installed && s.n_restarts != null && s.n_restarts > 0 && ` · 尝试重启 ${s.n_restarts} 次`}
              </div>
            </div>

            <span style={{ color: b.color, fontSize: 13, fontWeight: 600, whiteSpace: 'nowrap' }}>{b.text}</span>

            <label style={{ display: 'inline-flex', alignItems: 'center', gap: 5, color: '#9aa4b2',
                            fontSize: 12, whiteSpace: 'nowrap', cursor: 'pointer' }}
                   title="勾选后，仅当关机前最后状态为运行时才会在下次开机恢复">
              <input type="checkbox" checked={s.autostart} disabled={!!autostartBusy[s.key] || isBusy}
                style={{ margin: 0, accentColor: '#3b82f6' }}
                onChange={event => handleAutostart(s.key, event.target.checked)} />
              {autostartBusy[s.key] ? '保存中' : '开机自启'}
            </label>

            <div style={{ display: 'flex', gap: 6, whiteSpace: 'nowrap' }}>
              {needsInstall ? (
                <span
                  title={runningApp ? `将自动绑定到 ${runningApp.name}` : '请先在「程序管理」中启动视觉程序，后台服务会自动绑定后再启动'}
                  style={{ display: 'inline-flex', cursor: runningApp ? 'default' : 'not-allowed' }}
                >
                  <button
                    style={runningApp ? btn('#3b82f6') : { ...disabledStartBtn, pointerEvents: 'none' }}
                    disabled={isBusy || !runningApp}
                    // disabled 按钮不接收鼠标事件，交给外层 span 显示悬停提示。
                    onClick={() => act(s.key, () => controlService(s.key, 'start'),
                      `${s.label} 已自动绑定到 ${runningApp?.name ?? ''} 并启动`)}>
                    {isBusy ? '…' : (s.installed && !s.path_ok ? '🔧 自动修复并启动' : '自动绑定并启动')}
                  </button>
                </span>
              ) : (
                <>
                  {started ? (
                    <button style={btn('#ef4444')} disabled={isBusy}
                      onClick={() => act(s.key, () => controlService(s.key, 'stop'), `${s.label} 已停止`)}>
                      {isBusy ? '…' : '■ 停止'}
                    </button>
                  ) : (
                    <span
                      title={runningApp ? `将自动绑定到 ${runningApp.name}` : '请先在「程序管理」中启动视觉程序，后台服务会自动绑定后再启动'}
                      style={{ display: 'inline-flex', cursor: runningApp ? 'default' : 'not-allowed' }}
                    >
                      <button
                        style={runningApp ? btn('#22c55e') : { ...disabledStartBtn, pointerEvents: 'none' }}
                        disabled={isBusy || !runningApp}
                        onClick={() => act(s.key, () => controlService(s.key, 'start'),
                          `${s.label} 已自动绑定到 ${runningApp?.name ?? ''} 并启动`)}>
                        {isBusy ? '…' : '▶ 自动绑定并启动'}
                      </button>
                    </span>
                  )}
                  <button style={ghost} onClick={() => { setLogKey(s.key); setLogLines([]) }}>≡ 日志</button>
                </>
              )}
            </div>
          </div>
        )
      })}

      {logKey && (
        <div style={{ position: 'fixed', inset: 0, background: 'rgba(0,0,0,0.55)', zIndex: 1000,
                      display: 'flex', alignItems: 'center', justifyContent: 'center' }}
             onClick={() => setLogKey(null)}>
          <div style={{ background: '#0f1117', border: '1px solid #2e3352', borderRadius: 10,
                        width: 820, maxWidth: '94vw', height: '74vh', display: 'flex', flexDirection: 'column' }}
               onClick={e => e.stopPropagation()}>
            <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center',
                          padding: '12px 16px', borderBottom: '1px solid #2e3352' }}>
              <span style={{ fontWeight: 600 }}>≡ {services.find(s => s.key === logKey)?.label} · journalctl</span>
              <div style={{ display: 'flex', gap: 8 }}>
                <button style={ghost} onClick={handleClearLogs}>清空</button>
                <button style={ghost} onClick={() => {
                  autoScrollRef.current = true
                  setTimeout(scrollToBottom, 10)
                }}>跳到底部</button>
                <button style={ghost} onClick={() => setLogKey(null)}>关闭</button>
              </div>
            </div>
            <pre ref={logContainerRef} onScroll={handleLogScroll}
              style={{ flex: 1, overflow: 'auto', margin: 0, padding: '12px 16px',
                        fontSize: 12, lineHeight: 1.6, fontFamily: "'JetBrains Mono','Fira Code','Courier New',monospace",
                        color: '#94a3b8', background: '#080b14', whiteSpace: 'pre-wrap' }}>
              {logLines.length === 0 ? (
                <div style={{ color: '#9aa4b2', textAlign: 'center', padding: 40 }}>暂无日志…</div>
              ) : (
                logLines.map((line, i) => (
                  <div key={i} style={{
                    color: line.includes('[进程已停止]') || line.includes('ERROR') || line.includes('error') ? '#fca5a5'
                         : line.includes('WARN') ? '#fcd34d' : '#94a3b8'
                  }}>{line}</div>
                ))
              )}
            </pre>
          </div>
        </div>
      )}
    </div>
  )
}
