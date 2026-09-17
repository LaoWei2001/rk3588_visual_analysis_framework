import { useCallback, useEffect, useState } from 'react'
import { useNavigate, useParams } from 'react-router-dom'
import {
  deleteAllRecords, deleteRecord, fetchRecordJson, fetchRecords,
  recordImageUrl, recordVideoUrl, retryRecord,
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
  const [detailLoading, setDetailLoading] = useState(false)
  const [detailError, setDetailError] = useState('')
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
      setError('读取告警记录失败')
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
    setDetailLoading(true)
    setDetailError('')
    try { setDetail(await fetchRecordJson(appName, record.id)) }
    catch { setDetailError('读取这次算法数据失败') }
    finally { setDetailLoading(false) }
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
      <span className="records-title">{appName} — 告警记录与发送</span>
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
            {{ all: '全部', data: '仅数据', image: '图片', video: '视频' }[item]}
          </button>)}
      </div>
    </div>

    {loading ? <div className="records-empty">加载中…</div>
      : error ? <div className="records-empty err">{error}</div>
        : shown.length === 0 ? <div className="records-empty">当前没有告警记录。</div>
          : <div className="records-grid">{shown.map(record => {
            const failed = Object.entries(record.media_statuses ?? {})
              .filter(([, state]) => state.status === 'failed')
            const visibleDeliveries = record.deliveries.slice(0, 1)
            const mediaLabel = record.has_video ? '视频'
              : record.has_annotated_image || record.has_raw_image ? '图片' : '仅数据'
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
                        : record.required_media.length ? '媒体生成中…' : '仅数据，无图片或视频'}</span>
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
                    {record.message || record.event_type || '未命名告警'}
                  </div>
                  <span className="rec-channel">
                    {record.channel_id == null ? '全局逻辑' : `通道 ${record.channel_id}`}
                  </span>
                </div>
                <div className="rec-meta-row">
                  <span title={record.event_type}>{record.event_type || '未命名告警'}</span>
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
                          title={`${delivery.contract_label || '接口模板'} / ${delivery.connection_id || '未选择连接'}`}>
                          {delivery.contract_label || '接口模板'} / {delivery.connection_id || '未选择连接'}
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
                    onClick={() => openDetail(record)}>查看数据</button>{' '}
                  <button className="rec-btn" onClick={async () => {
                    await retryRecord(appName!, record.id); load()
                  }}>重试</button>{' '}
                  <button className="rec-btn rec-btn-danger" onClick={async () => {
                    if (confirm('确定删除这条告警记录？')) {
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
              {mediaRecord.channel_id == null ? '全局逻辑' : `通道 ${mediaRecord.channel_id}`} · {fmtTime(mediaRecord.snap_time)} · {mediaRecord.message || mediaRecord.event_type}
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
            <div className="record-modal-title">算法产生的数据</div>
            <div className="record-modal-subtitle">{detailRecord.id} · {detailRecord.event_type}</div>
          </div>
          <button className="rec-btn rec-btn-secondary" onClick={() => setDetailRecord(null)}>关闭</button>
        </div>
        {detailLoading ? <div className="record-json-state">正在读取…</div>
          : detailError ? <div className="record-json-state err">{detailError}</div>
            : detail && <div className="record-business-view">
              <div className="record-data-flow">
                <strong>算法产生的数据</strong><b>→</b><span>字段转换规则</span><b>→</b><span>最终发送请求</span>
              </div>
              <div className="record-json-hint">
                这里先展示算法本次产生的业务内容。服务器最终收到的内容请在画布上报节点中查看。
              </div>
              <div className="record-business-summary">
                <div><span>告警类型</span><strong>{String(detail.event.type ?? detailRecord.event_type ?? '—')}</strong></div>
                <div><span>说明</span><strong>{String(detail.event.message ?? detailRecord.message ?? '—')}</strong></div>
                <div><span>产生时间</span><strong>{String(detail.event.snap_time ?? detailRecord.snap_time ?? '—')}</strong></div>
                <div><span>来源</span><strong>{detailRecord.channel_id == null ? '全局逻辑' : `通道 ${detailRecord.channel_id}`}</strong></div>
              </div>

              <div className="record-business-section-title">算法字段</div>
              <div className="record-business-fields">
                {Object.entries(detail.fields).length === 0
                  ? <div className="record-business-empty">本次没有算法自定义字段。</div>
                  : Object.entries(detail.fields).map(([key, value]) => <div key={key}>
                    <span>{key}</span>
                    <strong>{typeof value === 'object' ? JSON.stringify(value) : String(value)}</strong>
                  </div>)}
              </div>

              <div className="record-business-section-title">发送情况</div>
              <div className="record-business-deliveries">
                {detail.deliveries.length === 0
                  ? <div className="record-business-empty">这次数据没有配置发送目标。</div>
                  : detail.deliveries.map((delivery, index) => {
                    const status = String(delivery.status ?? 'pending')
                    return <div key={String(delivery.id ?? index)}>
                      <span>{String(delivery.contract_label ?? '接口模板')}</span>
                      <strong className={`rec-delivery-status status-${status}`}>
                        {deliveryStatusText[status] || status}
                      </strong>
                    </div>
                  })}
              </div>

              <details className="record-json-advanced">
                <summary>高级诊断：查看完整原始数据</summary>
                <p>包含媒体状态、发送任务和系统时间戳等排障信息，普通接口配置无需关注。</p>
                <pre className="record-json-code">{JSON.stringify(detail, null, 2)}</pre>
              </details>
            </div>}
      </div>
    </div>}
  </div>
}
