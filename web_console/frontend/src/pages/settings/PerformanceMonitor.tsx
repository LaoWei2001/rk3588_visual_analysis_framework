import { useEffect, useRef, useState } from 'react'
import { resetPerformance, savePerformance } from '../../api/client'
import type { DeviceInformation, PerformanceSnapshot } from '../../api/client'
import ConfigSyncNotice from '../../components/ConfigSyncNotice'

const usage = (v?: number | null) => v == null ? '未提供' : `${v.toFixed(1)}%`
const memorySize = (bytes?: number | null) => bytes == null ? '未提供' : `${(bytes / 1024 ** 3).toFixed(2)}GiB`
const mhz = (v?: number | null) => v == null ? '未提供' : `${v.toFixed(0)} MHz`
const temp = (v?: number | null) => v == null ? '未提供' : `${v.toFixed(1)} ℃`
const tripLabels = { passive: '降频阈值', hot: '高温保护阈值', critical: '临界保护阈值' }
type Resource = 'CPU' | 'GPU' | 'NPU' | 'memory'
type Point = Record<Resource, number | null> & { time: number; npuCores: Partial<Record<number, number>> }
const npuCores = [
  { core: 0, color: '#38bdf8', dash: undefined },
  { core: 1, color: '#fb923c', dash: '8 4' },
  { core: 2, color: '#a78bfa', dash: '2 4' },
]
const resources: Resource[] = ['CPU', 'NPU', 'GPU', 'memory']
const titles = { CPU: 'CPU', GPU: 'GPU', NPU: 'NPU', memory: '内存' }
function graph(points: Point[], resource: Resource | number, width: number, height: number) {
  let drawing = false
  return points.map((point, i) => {
    const value = typeof resource === 'number' ? point.npuCores[resource] : point[resource]
    if (value == null) { drawing = false; return '' }
    const command = drawing ? 'L' : 'M'; drawing = true
    return `${command}${((60 - points.length + i) / 59 * width).toFixed(1)},${(height * (1 - value / 100)).toFixed(1)}`
  }).join(' ')
}
function PerformanceSettings({ value, onSaved }: { value: PerformanceSnapshot; onSaved: (v: PerformanceSnapshot) => void }) {
  const [draft, setDraft] = useState<Record<string, string> | null>(null)
  const revision = useRef('')
  const [busy, setBusy] = useState(false)
  const [feedback, setFeedback] = useState('')
  const [failed, setFailed] = useState(false)
  const limits: Record<string, string> = {}
  value.devices.forEach(d => { if (d.editable) limits[d.id] = String(d.max_mhz) })
  value.thermal_trips.forEach(t => { if (t.editable) limits[t.id] = String(t.celsius) })
  const current = draft || limits
  const conflict = !!draft && revision.current !== value.revision
  const edit = (key: string, val: string) => {
    if (!draft) revision.current = value.revision
    setDraft({ ...current, [key]: val }); setFeedback('')
  }
  const apply = async (reset = false) => {
    setBusy(true); setFeedback(''); setFailed(false)
    try {
      const payload = Object.fromEntries(Object.entries(current).map(([key, val]) => {
        if (!val.trim() || !Number.isFinite(Number(val))) throw new Error('请填写有效的温度阈值')
        return [key, Number(val)]
      }))
      const result = reset ? await resetPerformance(draft ? revision.current : value.revision)
        : await savePerformance(payload, draft ? revision.current : value.revision)
      onSaved(result); setDraft(null)
      setFeedback(reset ? '已恢复初始设置。' : '已保存并生效。')
    } catch (error) {
      const e = error as { response?: { data?: { detail?: string } }; message?: string }
      setFailed(true); setFeedback(e.response?.data?.detail || e.message || '保存失败')
    } finally { setBusy(false) }
  }
  return <details className="performance-settings"><summary>性能与温控设置</summary>
    <ConfigSyncNotice conflict={conflict} onReload={() => { setDraft(null); setFeedback('') }} />
    {value.error && <p className="device-settings-message err">{value.error}</p>}
    {value.notice && <p className="device-settings-note warning">{value.notice}</p>}
    <h5>频率上限</h5><div className="performance-controls">{value.devices.map(d => <label key={d.id}><span>{d.label}</span>
      {d.editable ? <select aria-label={`${d.label}最大频率`} value={current[d.id] ?? ''} disabled={busy} onChange={e => edit(d.id, e.target.value)}>
        {!d.available_mhz.includes(Number(current[d.id])) && <option value={current[d.id]}>{mhz(Number(current[d.id]))}（当前值）</option>}
        {d.available_mhz.map(f => <option key={f} value={f}>{mhz(f)}</option>)}
      </select> : <span>上限 {mhz(d.max_mhz)} · 只读</span>}</label>)}</div>
    {!value.devices.length && <p className="recovery-help">未提供频率信息。</p>}
    <h5>温控阈值</h5><div className="performance-controls">{value.thermal_trips.map(t => <label key={t.id}>
      <span>{t.zone} · {tripLabels[t.kind]}{t.ceiling_celsius != null && <small>上限 {temp(t.ceiling_celsius)}</small>}</span>
      {t.editable ? <div className="performance-temperature-input"><input type="number" aria-label={`${t.zone} ${tripLabels[t.kind]} ${t.id}`} min="20" max={t.ceiling_celsius ?? 150} step="0.1" value={current[t.id] ?? ''} disabled={busy} onChange={e => edit(t.id, e.target.value)} /><span>℃</span></div>
        : <span>{temp(t.celsius)} · 仅可读取</span>}</label>)}</div>
    {!value.thermal_trips.length && <p className="recovery-help">未提供可配置的温控阈值。</p>}
    {value.thermal_trips.some(t => t.kind === 'critical') && <p className="recovery-help">临界温度可能触发关机或重启。</p>}
    <div className="recovery-agent-actions">
      <button type="button" className="settings-primary-btn" disabled={busy || !draft || conflict || !!value.error} onClick={() => void apply()}>{busy ? '应用中……' : '保存并应用'}</button>
      <button type="button" className="settings-ghost-btn" disabled={busy || !draft} onClick={() => { setDraft(null); setFeedback('') }}>放弃修改</button>
      <button type="button" className="settings-ghost-btn" disabled={busy || !value.saved || !value.profile_matches || !!value.error} onClick={() => void apply(true)}>恢复初始设置</button>
    </div>
    {draft && <p className="recovery-help">有未保存修改。</p>}
    {feedback && <p role="status" className={`device-settings-message ${failed ? 'err' : 'ok'}`}>{feedback}</p>}
  </details>
}
export default function PerformanceMonitor({ information, onSaved }: { information: DeviceInformation; onSaved: (v: PerformanceSnapshot) => void }) {
  const [selected, setSelected] = useState<Resource>('CPU')
  const [points, setPoints] = useState<Point[]>([])
  const perf = information.performance
  useEffect(() => {
    const average = (kind: 'GPU' | 'NPU') => {
      const values = information.performance?.devices.filter(d => d.kind === kind && d.usage_percent != null).map(d => d.usage_percent!) || []
      return values.length ? values.reduce((sum, v) => sum + v, 0) / values.length : null
    }
    const coreLoads = information.performance?.devices.find(d => d.kind === 'NPU')?.core_loads || []
    const readings: Point['npuCores'] = {}
    for (const reading of coreLoads) {
      if (npuCores.some(item => item.core === reading.core) && Number.isFinite(reading.usage_percent)
        && reading.usage_percent >= 0 && reading.usage_percent <= 100) readings[reading.core] = reading.usage_percent
    }
    const next = { npuCores: readings, time: information.updated_at, CPU: information.cpu.usage_percent ?? null,
      GPU: average('GPU'), NPU: average('NPU'), memory: information.memory.usage_percent ?? null }
    setPoints(previous => previous[previous.length - 1]?.time === next.time ? previous : [...previous, next].slice(-60))
  }, [information])
  const devices = perf?.devices.filter(d => d.kind === selected) || []
  const latest = points[points.length - 1]
  const resourceTemperature = (resource: Resource) => {
    const values = perf?.devices.filter(device => device.kind === resource)
      .map(device => device.temperature_celsius).filter((value): value is number => value != null && Number.isFinite(value)) || []
    return values.length ? Math.max(...values) : null
  }
  const currentTemperature = resourceTemperature(selected)
  const frequencies = devices.map(d => d.current_mhz).filter((v): v is number => v != null)
  return <><div className="performance-monitor">
    <div className="performance-resource-list" role="tablist" aria-label="性能资源">{resources.map(resource => <button type="button" key={resource} role="tab" aria-selected={selected === resource} onClick={() => setSelected(resource)}>
      <svg viewBox="0 0 90 40" aria-hidden="true">{resource === 'NPU' ? npuCores.map(item =>
        <path key={item.core} d={graph(points, item.core, 90, 40)} style={{ stroke: item.color }} strokeDasharray={item.dash} />)
        : <path d={graph(points, resource, 90, 40)} />}</svg><span><b>{titles[resource]}</b><small>{resource === 'memory'
          ? `${memorySize(information.memory.used_bytes)}/${memorySize(information.memory.total_bytes)} (${usage(information.memory.usage_percent)})`
          : `${usage(latest?.[resource])}（${resourceTemperature(resource) == null ? '温度未提供' : resourceTemperature(resource)!.toFixed(1) + '℃'}）`}</small></span>
    </button>)}</div>
    <div className="performance-detail" role="tabpanel" aria-label={`${titles[selected]}性能`}>
      <div className="performance-heading"><h3>{titles[selected]}</h3><span>{selected === 'CPU' ? `${information.cpu.cores ?? '—'} 核` : selected === 'memory' ? `${information.memory.total_bytes == null ? '未提供' : (information.memory.total_bytes / 1024 ** 3).toFixed(1) + ' GiB'}` : devices.map(d => d.driver).filter(Boolean).join(' / ')}</span></div>
      <div className="performance-graph-label"><span>{selected === 'NPU' ? '各核心利用率' : '利用率'}</span><span>100%</span></div>
      <svg className="performance-graph" viewBox="0 0 600 180" preserveAspectRatio="none" role="img" aria-label={`${titles[selected]}最近60次采样利用率曲线`} data-samples={points.length}>
        {Array.from({ length: 13 }, (_, i) => <line key={`x${i}`} x1={i * 50} x2={i * 50} y1="0" y2="180" />)}
        {Array.from({ length: 5 }, (_, i) => <line key={`y${i}`} y1={i * 45} y2={i * 45} x1="0" x2="600" />)}
        {selected === 'NPU' ? npuCores.map(item => <path key={item.core} data-npu-core={item.core}
          aria-label={`NPU 核心 ${item.core}利用率`} d={graph(points, item.core, 600, 180)}
          style={{ stroke: item.color }} strokeDasharray={item.dash} />) : <path d={graph(points, selected, 600, 180)} />}
      </svg>
      <div className="performance-graph-label"><span>最近 60 次</span><span>现在</span></div>
      {selected === 'NPU' && <ul className="performance-core-legend" aria-label="NPU 核心图例">{npuCores.map(item =>
        <li key={item.core} style={{ color: item.color }}><svg viewBox="0 0 24 8" aria-hidden="true">
          <line x1="0" x2="24" y1="4" y2="4" strokeDasharray={item.dash} /></svg>
          <span>核心 {item.core}：{usage(latest?.npuCores[item.core])}</span></li>)}</ul>}
      <div className="performance-metrics"><div><span>{selected === 'NPU' ? '平均利用率' : '利用率'}</span><strong>{usage(latest?.[selected])}</strong></div>
        {selected === 'memory' ? <div><span>已用 / 总共</span><strong>{information.memory.used_bytes == null ? '未提供' : (information.memory.used_bytes / 1024 ** 3).toFixed(2) + ' GiB'} / {information.memory.total_bytes == null ? '未提供' : (information.memory.total_bytes / 1024 ** 3).toFixed(2) + ' GiB'}</strong></div> : <>
          <div><span>当前频率{devices.length > 1 ? '范围' : ''}</span><strong>{frequencies.length ? frequencies.length > 1 ? `${Math.min(...frequencies)}～${Math.max(...frequencies)} MHz` : mhz(frequencies[0]) : '未提供'}</strong></div>
          <div><span>温度</span><strong>{temp(currentTemperature)}</strong></div>
        </>}
      </div>
      {devices.map(d => <div className="performance-device-detail" key={d.id}>{devices.length > 1 && <b>{d.label}</b>}
        <span>{devices.length > 1 && `当前 ${mhz(d.current_mhz)} · `}上限 {mhz(d.max_mhz)}</span></div>)}
      {selected !== 'memory' && selected !== 'CPU' && !devices.some(d => d.usage_percent != null) && <p className="recovery-help">{titles[selected]}利用率未提供。</p>}
    </div>
  </div>{perf ? <PerformanceSettings value={perf} onSaved={onSaved} /> : <p className="recovery-help">性能设置暂不可用。</p>}</>
}
