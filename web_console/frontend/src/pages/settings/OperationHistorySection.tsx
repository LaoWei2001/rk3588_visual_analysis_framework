import { useEffect, useState } from 'react'
import { fetchRecoveryHistory } from '../../api/client'
import type { RecoveryHistorySnapshot } from '../../api/client'
import { isFaultRecoveryEvent, RecoveryEventList } from './RecoveryHistory'
import { readTimestamp } from './recoveryFeedback'
import './RecoverySettingsSection.css'

const errorText = (failure: unknown) => {
  const value = failure as { response?: { data?: { detail?: string } }; message?: string }
  return value.response?.data?.detail || value.message || '读取失败'
}

export default function OperationHistorySection({ active }: { active: boolean }) {
  const [history, setHistory] = useState<RecoveryHistorySnapshot | null>(null)
  const [category, setCategory] = useState('all')
  const [fetching, setFetching] = useState(false)
  const [error, setError] = useState('')
  const [readAt, setReadAt] = useState('')

  useEffect(() => {
    if (!active) return
    let alive = true
    let pending = false
    const refresh = async () => {
      if (pending) return
      pending = true
      setFetching(true)
      try {
        const value = await fetchRecoveryHistory()
        if (alive) { setHistory(value); setError(''); setReadAt(readTimestamp()) }
      } catch (failure) {
        if (alive) setError(errorText(failure))
      } finally {
        pending = false
        if (alive) setFetching(false)
      }
    }
    void refresh()
    const timer = window.setInterval(() => { void refresh() }, 5000)
    return () => { alive = false; window.clearInterval(timer) }
  }, [active])

  const keep = history?.keep_operations ?? 20
  const records = history?.events.filter(event => !isFaultRecoveryEvent(event)).slice(0, keep) ?? []
  const visible = records.filter(event => category === 'all' || event.category === category)
  const observer = history?.observer
  const stale = !!history && (!observer?.updated_at || Date.now() / 1000 - observer.updated_at > 60)

  return <section className="device-settings-card recovery-settings operation-settings">
    <div className="device-settings-card-head"><div><h4>运行与操作记录</h4>
      <p>保留最新 {keep} 条，故障记录见对应恢复分类。</p>
    </div></div>
    <div className="recovery-agent-actions operation-history-controls">
      <label>记录类别 <select aria-label="记录类别" value={category} onChange={event => setCategory(event.target.value)}>
        <option value="all">全部类别</option><option value="system">系统</option>
        <option value="network">网络</option><option value="application">程序</option>
      </select></label>
      {!!readAt && <small className="recovery-read-feedback">上次读取：{readAt}</small>}
    </div>
    {error && <div className="device-settings-message err">记录读取失败：{error}{history ? '。当前显示上次读取的记录。' : ''}</div>}
    {history?.error && <div className="device-settings-message err">记录文件读取失败：{history.error}</div>}
    {observer?.error && <div className="device-settings-message err">记录服务异常：{observer.error}</div>}
    {stale && <div className="device-settings-note warning">尚未确认记录服务正在运行，新事件可能未被记录。</div>}
    <div className="recovery-events recovery-history operation-history">
      {!!visible.length ? <><p>当前显示 {visible.length} 条</p><RecoveryEventList events={visible} operation /></>
        : !error && !history?.error && <p>{fetching && !history ? '正在读取运行与操作记录……' : '暂无符合条件的运行与操作记录。'}</p>}
    </div>
  </section>
}
