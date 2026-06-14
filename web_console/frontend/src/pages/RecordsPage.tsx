import { useEffect, useState, useCallback } from 'react'
import { useParams, useNavigate } from 'react-router-dom'
import { fetchRecords, recordImageUrl, AlarmRecord } from '../api/client'
import './RecordsPage.css'

function fmtBytes(n: number): string {
  if (n < 1024) return `${n} B`
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(0)} KB`
  return `${(n / 1024 / 1024).toFixed(1)} MB`
}

export default function RecordsPage() {
  const { appName } = useParams()
  const navigate = useNavigate()
  const [records, setRecords] = useState<AlarmRecord[]>([])
  const [count, setCount] = useState(0)
  const [totalBytes, setTotalBytes] = useState(0)
  const [capBytes, setCapBytes] = useState(0)
  const [loading, setLoading] = useState(true)
  const [err, setErr] = useState('')

  const load = useCallback(async () => {
    if (!appName) return
    try {
      const data = await fetchRecords(appName, 500)
      setRecords(data.records)
      setCount(data.count)
      setTotalBytes(data.total_bytes)
      setCapBytes(data.cap_bytes)
      setErr('')
    } catch {
      setErr('读取未上报告警失败')
    } finally {
      setLoading(false)
    }
  }, [appName])

  useEffect(() => {
    load()
    // 自动刷新：补传成功的会从后端消失，这里就会自动少一条
    const id = setInterval(load, 4000)
    return () => clearInterval(id)
  }, [load])

  return (
    <div className="records-page">
      <div className="records-header">
        <button className="rec-btn" onClick={() => navigate('/')}>← 返回</button>
        <span className="records-title">{appName} — 未上报告警</span>
        <span className="records-stat">
          {count} 条 · {fmtBytes(totalBytes)}{capBytes ? ` / ${fmtBytes(capBytes)}` : ''}
        </span>
        <button className="rec-btn" style={{ marginLeft: 'auto' }} onClick={load}>↻ 刷新</button>
      </div>

      <div className="records-tip">
        这里只显示<b>平台还没收到</b>的告警（断网 / 上报服务没开时暂存在盒子里的）。
        上传通道一旦恢复，传成功的会自动从这里消失。
      </div>

      {loading ? (
        <div className="records-empty">加载中…</div>
      ) : err ? (
        <div className="records-empty err">{err}</div>
      ) : records.length === 0 ? (
        <div className="records-empty">暂无未上报记录（说明都已成功上报，或当前无告警）。</div>
      ) : (
        <div className="records-grid">
          {records.map(r => (
            <a
              key={r.id}
              className="rec-card"
              href={recordImageUrl(appName!, r.id, false)}
              target="_blank"
              rel="noreferrer"
              title="点击查看大图"
            >
              <img className="rec-thumb" src={recordImageUrl(appName!, r.id, false)} alt="" loading="lazy" />
              <div className="rec-info">
                <span className="rec-badge">未上报</span>
                <span className="rec-line">通道 {r.camera_id ?? '?'} · {r.alarm_type || '—'}</span>
                <span className="rec-time">{r.snapTime}</span>
              </div>
            </a>
          ))}
        </div>
      )}
    </div>
  )
}
