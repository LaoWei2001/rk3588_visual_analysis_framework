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

const deliveryStatusText: Record<string, string> = {
  pending: '尝试发送',
  uploading: '发送中',
  retry: '等待重试',
  delivered: '已送达',
  failed: '发送失败',
  invalid: '配置无效',
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
  const [mediaRecord, setMediaRecord] = useState<EventRecord | null>(null)
  const [showRawImage, setShowRawImage] = useState(false)

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

  useEffect(() => {
    const closeModal = (event: KeyboardEvent) => {
      if (event.key !== 'Escape') return
      setMediaRecord(null)
      setDetailRecord(null)
    }
    window.addEventListener('keydown', closeModal)
    return () => window.removeEventListener('keydown', closeModal)
  }, [])

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

  const openRecord = (record: EventRecord) => {
    if (record.has_video || record.has_annotated_image || record.has_raw_image) {
      setShowRawImage(!record.has_annotated_image && record.has_raw_image)
      setMediaRecord(record)
      return
    }
    openDetail(record)
  }

  return <div className="records-page">
    <div className="records-header">
      <button className="rec-btn" onClick={() => navigate('/')}>← 返回</button>
      <span className="records-title">{appName} — 本地事件发件箱</span>
      <span className="records-stat">
        {stats.count} 条 · {fmtBytes(stats.total)}{stats.cap ? ` / ${fmtBytes(stats.cap)}` : ''}
      </span>
      <div className="records-header-actions">
        <button className="rec-btn" onClick={load}>↻ 刷新</button>
        <button className="rec-btn rec-btn-danger" disabled={clearing || stats.count === 0}
          onClick={async () => {
            if (!appName || !confirm(`确定清空全部 ${stats.count} 条待上报记录？此操作不可撤销。`)) return
            setClearing(true)
            try { await deleteAllRecords(appName); load() }
            catch { setError('清空失败') }
            finally { setClearing(false) }
          }}>{clearing ? '清空中…' : '清空全部'}
        </button>
      </div>
    </div>
    <div className="records-toolbar">
      <div className="records-filters">
        {(['all', 'data', 'image', 'video'] as const).map(item =>
          <button key={item} className={`rec-btn ${filter === item ? 'active' : ''}`}
            onClick={() => setFilter(item)}>
            {{ all: '全部', data: '仅事件数据', image: '图片', video: '视频' }[item]}
          </button>)}
      </div>
    </div>

    {loading ? <div className="records-empty">加载中…</div>
      : error ? <div className="records-empty err">{error}</div>
        : shown.length === 0 ? <div className="records-empty">当前没有待投递事件。</div>
          : <div className="records-grid">{shown.map(record => {
            const failed = Object.entries(record.media_statuses ?? {})
              .filter(([, state]) => state.status === 'failed')
            const visibleDeliveries = record.deliveries.slice(0, 1)
            const mediaLabel = record.has_video ? '视频'
              : record.has_annotated_image || record.has_raw_image ? '图片' : '事件数据'
            return <article key={record.id} className="rec-card rec-card-clickable"
              role="button" tabIndex={0} onClick={() => openRecord(record)}
              onKeyDown={event => {
                if (event.target !== event.currentTarget) return
                if (event.key === 'Enter' || event.key === ' ') openRecord(record)
              }}>
              <div className="rec-media">
                {record.has_video
                  ? <video className="rec-thumb" src={recordVideoUrl(appName!, record.id)}
                    muted playsInline preload="metadata" aria-hidden="true" />
                  : record.has_annotated_image || record.has_raw_image
                    ? <img className="rec-thumb" loading="lazy"
                      src={recordImageUrl(appName!, record.id, !record.has_annotated_image)} alt="" />
                    : <div className={`rec-thumb rec-placeholder ${failed.length ? 'err' : ''}`}>
                      <span>{failed.length ? '媒体生成失败'
                        : record.required_media.length ? '媒体生成中…' : '仅事件数据'}</span>
                    </div>}
                <div className="rec-media-overlay">
                  <span className={`rec-media-type ${record.has_video ? 'video' : 'image'}`}>
                    {record.has_video ? '▶' : record.has_annotated_image || record.has_raw_image ? '▧' : '⌘'} {mediaLabel}
                  </span>
                </div>
              </div>
              <div className="rec-info">
                <div className="rec-card-heading">
                  <div className="rec-card-message" title={record.message || ''}>
                    {record.message || record.event_type || '未命名事件'}
                  </div>
                  <span className="rec-channel">通道 {record.channel_id ?? '?'}</span>
                </div>
                <div className="rec-meta-row">
                  <span title={record.event_type}>{record.event_type || '未命名事件'}</span>
                  <span>{fmtTime(record.snap_time)}</span>
                  {(record.trigger_count ?? 1) > 1 &&
                    <span>合并 {record.trigger_count} 次</span>}
                </div>
                <div className="rec-deliveries">
                  {visibleDeliveries.map((delivery, index) => {
                    const status = delivery.status || 'pending'
                    return <div className="rec-delivery" key={delivery.id ?? index}>
                      <div className="rec-delivery-head">
                        <span className="rec-delivery-name"
                          title={`${delivery.contract_id || '未选择接口契约'} / ${delivery.profile_id || '未选择连接'}`}>
                          {delivery.contract_id || '未选择接口契约'} / {delivery.profile_id || '未选择连接'}
                        </span>
                        <span className={`rec-delivery-status status-${status}`}>
                          {deliveryStatusText[status] || status} · {delivery.attempts ?? 0}次
                        </span>
                      </div>
                      {delivery.last_error && <div className="rec-delivery-error"
                        title={delivery.last_error}>{delivery.last_error}</div>}
                    </div>
                  })}
                  {record.deliveries.length > visibleDeliveries.length &&
                    <div className="rec-more-deliveries">
                      另有 {record.deliveries.length - visibleDeliveries.length} 个投递目标
                    </div>}
                </div>
                <div className="rec-actions" onClick={event => event.stopPropagation()}>
                  <button className="rec-btn rec-btn-secondary"
                    onClick={() => openDetail(record)}>事件数据</button>{' '}
                  <button className="rec-btn" onClick={async () => {
                    await retryRecord(appName!, record.id); load()
                  }}>重试</button>{' '}
                  <button className="rec-btn rec-btn-danger" onClick={async () => {
                    if (confirm('确定删除这条本地事件？')) {
                      await deleteRecord(appName!, record.id); load()
                    }
                  }}>删除</button>
                </div>
              </div>
            </article>
          })}</div>}

    {mediaRecord && <div className="record-modal-backdrop" onClick={() => setMediaRecord(null)}>
      <div className="record-media-dialog" role="dialog" aria-modal="true"
        onClick={event => event.stopPropagation()}>
        <div className="record-modal-header">
          <div className="record-modal-heading">
            <div className="record-modal-title">
              {mediaRecord.has_video ? '告警视频' : '告警图片'}
            </div>
            <div className="record-modal-subtitle">
              通道 {mediaRecord.channel_id ?? '?'} · {fmtTime(mediaRecord.snap_time)} · {mediaRecord.message || mediaRecord.event_type}
            </div>
          </div>
          <button className="rec-btn rec-btn-secondary" onClick={() => setMediaRecord(null)}>关闭</button>
        </div>
        {!mediaRecord.has_video && mediaRecord.has_annotated_image && mediaRecord.has_raw_image &&
          <div className="record-media-toolbar">
            <button className={`rec-btn ${showRawImage ? '' : 'active'}`}
              onClick={() => setShowRawImage(false)}>标注图片</button>
            <button className={`rec-btn ${showRawImage ? 'active' : ''}`}
              onClick={() => setShowRawImage(true)}>原始图片</button>
          </div>}
        <div className="record-media-stage">
          {mediaRecord.has_video
            ? <video key={mediaRecord.id} className="record-media-content"
              src={recordVideoUrl(appName!, mediaRecord.id)} controls autoPlay playsInline />
            : <img className="record-media-content"
              src={recordImageUrl(appName!, mediaRecord.id, showRawImage)}
              alt={mediaRecord.message || '告警图片'} />}
        </div>
      </div>
    </div>}

    {detailRecord && <div className="record-modal-backdrop" onClick={() => setDetailRecord(null)}>
      <div className="record-json-dialog" onClick={event => event.stopPropagation()}>
        <div className="record-modal-header">
          <div className="record-modal-heading">
            <div className="record-modal-title">标准事件数据</div>
            <div className="record-modal-subtitle">{detailRecord.id} · {detailRecord.event_type}</div>
          </div>
          <button className="rec-btn rec-btn-secondary" onClick={() => setDetailRecord(null)}>关闭</button>
        </div>
        <div className="record-json-hint">
          这里展示适配器映射前的稳定事件结构；远端请求可在上报节点中预览。
        </div>
        <pre className="record-json-code">{JSON.stringify(detail, null, 2)}</pre>
      </div>
    </div>}
  </div>
}
