import { useCallback, useEffect, useState } from 'react'
import { useNavigate, useParams } from 'react-router-dom'
import {
  deleteAllRecords, deleteRecord, fetchRecordJson, fetchRecords, recordImageUrl, recordVideoUrl, retryRecord,
  type EventRecord, type RecordJsonResponse,
} from '../api/client'
import './RecordsPage.css'

function fmtBytes(value: number): string {
  if (value < 1024) return `${value} B`
  if (value < 1024 * 1024) return `${(value / 1024).toFixed(0)} KB`
  return `${(value / 1024 / 1024).toFixed(1)} MB`
}

function fmtTime(value: string | number): string {
  if (typeof value === 'string') return value
  return value ? new Date(value).toLocaleString() : '—'
}

export default function RecordsPage() {
  const { appName } = useParams()
  const navigate = useNavigate()
  const [records, setRecords] = useState<EventRecord[]>([])
  const [stats, setStats] = useState({ count: 0, total: 0, cap: 0 })
  const [filter, setFilter] = useState<'all' | 'data' | 'image' | 'video'>('all')
  const [loading, setLoading] = useState(true)
  const [clearing, setClearing] = useState(false)
  const [error, setError] = useState('')
  const [detailRecord, setDetailRecord] = useState<EventRecord | null>(null)
  const [detail, setDetail] = useState<RecordJsonResponse | null>(null)

  const load = useCallback(async () => {
    if (!appName) return
    try {
      const result = await fetchRecords(appName)
      setRecords(result.records)
      setStats({ count: result.count, total: result.total_bytes, cap: result.cap_bytes })
      setError('')
    } catch {
      setError('读取本地事件失败')
    } finally {
      setLoading(false)
    }
  }, [appName])

  useEffect(() => {
    load()
    const timer = setInterval(load, 4000)
    return () => clearInterval(timer)
  }, [load])

  const shown = records.filter(record => filter === 'all'
    || (filter === 'data' && record.required_media.length === 0)
    || (filter === 'image' && record.required_media.some(item => item.endsWith('_image')))
    || (filter === 'video' && record.required_media.includes('video')))

  const openDetail = async (record: EventRecord) => {
    if (!appName) return
    setDetailRecord(record)
    setDetail(null)
    try { setDetail(await fetchRecordJson(appName, record.id)) }
    catch { setDetail(null) }
  }

  return <div className="records-page">
    <div className="records-header">
      <button className="rec-btn" onClick={() => navigate('/')}>← 返回</button>
      <span className="records-title">{appName} — 本地事件发件箱</span>
      <span className="records-stat">
        {stats.count} 条 · {fmtBytes(stats.total)}{stats.cap ? ` / ${fmtBytes(stats.cap)}` : ''}
      </span>
      <button className="rec-btn" style={{ marginLeft: 'auto' }} onClick={load}>↻ 刷新</button>
      <button className="rec-btn" disabled={clearing || stats.count === 0}
        style={clearing ? {} : { background: '#7f1d1d', color: '#fca5a5', borderColor: '#991b1b' }}
        onClick={async () => {
          if (!appName || !confirm(`确定清空全部 ${stats.count} 条待上报记录？此操作不可撤销。`)) return
          setClearing(true)
          try { await deleteAllRecords(appName); load() }
          catch { setError('清空失败') }
          finally { setClearing(false) }
        }}>{clearing ? '清空中…' : '清空全部'}
      </button>
    </div>
    <div className="records-tip">
      {(['all', 'data', 'image', 'video'] as const).map(item =>
        <button key={item} className="rec-btn" onClick={() => setFilter(item)}>
          {{ all: '全部', data: '仅事件数据', image: '图片', video: '视频' }[item]}
        </button>)}
    </div>
    <div className="records-tip">
      这里只显示尚未全部投递成功的标准事件。算法类型、适配器和媒体组合不影响记录页结构。
    </div>

    {loading ? <div className="records-empty">加载中…</div>
      : error ? <div className="records-empty err">{error}</div>
        : shown.length === 0 ? <div className="records-empty">当前没有待投递事件。</div>
          : <div className="records-grid">{shown.map(record => {
            const failed = Object.entries(record.media_statuses ?? {})
              .filter(([, state]) => state.status === 'failed')
            return <div key={record.id} className="rec-card rec-card-clickable"
              onClick={() => openDetail(record)}>
              {record.has_video
                ? <video className="rec-thumb" src={recordVideoUrl(appName!, record.id)}
                  controls preload="metadata" onClick={event => event.stopPropagation()} />
                : record.has_annotated_image
                  ? <img className="rec-thumb" src={recordImageUrl(appName!, record.id)} alt="" />
                  : <div className={`rec-thumb records-empty ${failed.length ? 'err' : ''}`}>
                    {failed.length ? '媒体生成失败'
                      : record.required_media.length ? '媒体生成中…' : '仅事件数据'}
                  </div>}
              <div className="rec-info">
                <div className="rec-badges">
                  <span className="rec-badge">{record.event_type || '未命名事件'}</span>
                  <span className="rec-badge">{record.state}</span>
                </div>
                <span className="rec-line">通道 {record.channel_id ?? '?'} · {record.message || '—'}</span>
                <span className="rec-time">{fmtTime(record.snap_time)}</span>
                {(record.trigger_count ?? 1) > 1 &&
                  <span className="rec-time">已合并 {record.trigger_count} 次触发</span>}
                {record.deliveries.map((delivery, index) =>
                  <span className="rec-time" key={delivery.id ?? index} title={delivery.last_error ?? ''}>
                    {delivery.contract_id || '未选择接口契约'} / {delivery.profile_id}: {delivery.status}
                    （{delivery.attempts ?? 0}次）
                    {delivery.last_error ? ` · ${delivery.last_error}` : ''}
                  </span>)}
                <span onClick={event => event.stopPropagation()}>
                  <button className="rec-btn" onClick={() => openDetail(record)}>查看事件</button>{' '}
                  <button className="rec-btn" onClick={async () => {
                    await retryRecord(appName!, record.id); load()
                  }}>重试</button>{' '}
                  <button className="rec-btn" onClick={async () => {
                    if (confirm('确定删除这条本地事件？')) {
                      await deleteRecord(appName!, record.id); load()
                    }
                  }}>删除</button>
                </span>
              </div>
            </div>
          })}</div>}

    {detailRecord && <div className="record-json-backdrop" onClick={() => setDetailRecord(null)}>
      <div className="record-json-dialog" onClick={event => event.stopPropagation()}>
        <div className="record-json-header">
          <div>
            <div className="record-json-title">标准事件数据</div>
            <div className="record-json-subtitle">{detailRecord.id} · {detailRecord.event_type}</div>
          </div>
          <button className="rec-btn" onClick={() => setDetailRecord(null)}>关闭</button>
        </div>
        <div className="record-json-hint">
          这里展示适配器映射前的稳定事件结构；远端请求可在上报节点中预览。
        </div>
        <pre className="record-json-code">{JSON.stringify(detail, null, 2)}</pre>
      </div>
    </div>}
  </div>
}
