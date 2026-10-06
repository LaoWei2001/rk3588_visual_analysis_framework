import { useCallback, useEffect, useState } from 'react'
import { useNavigate, useParams } from 'react-router-dom'
import {
  apiErrorMessage, deleteAllRecords, deleteRecord, deleteSelectedRecords, fetchRecordJson, fetchRecords,
  prepareRawImageExport, rawImageDownloadUrl, rawImageExportUrl,
  recordImageUrl, recordVideoUrl, retryRecord,
  type EventRecord, type RecordJsonResponse,
} from '../api/client'
import './RecordsPage.css'
import { chooseLocalDirectory, downloadToComputer, isSaveCancelled, localDownloadHint, writeResponseToFile } from '../utils/localFiles'

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

type TimePreset = 'all' | 'today' | '24h' | '7d' | 'custom'

function toLocalDateTime(value: Date): string {
  const offset = value.getTimezoneOffset() * 60_000
  return new Date(value.getTime() - offset).toISOString().slice(0, 16)
}

export default function RecordsPage() {
  const { appName } = useParams()
  const navigate = useNavigate()
  const [records, setRecords] = useState<EventRecord[]>([])
  const [stats, setStats] = useState({ count: 0, total: 0, cap: 0 })
  const [filter, setFilter] = useState<'all' | 'data' | 'image' | 'video'>('all')
  const [timePreset, setTimePreset] = useState<TimePreset>('all')
  const [startTime, setStartTime] = useState('')
  const [endTime, setEndTime] = useState('')
  const [filteredCount, setFilteredCount] = useState(0)
  const [loading, setLoading] = useState(true)
  const [clearing, setClearing] = useState(false)
  const [error, setError] = useState('')
  const [detailRecord, setDetailRecord] = useState<EventRecord | null>(null)
  const [detail, setDetail] = useState<RecordJsonResponse | null>(null)
  const [detailLoading, setDetailLoading] = useState(false)
  const [detailError, setDetailError] = useState('')
  const [mediaRecord, setMediaRecord] = useState<EventRecord | null>(null)
  const [showRawImage, setShowRawImage] = useState(false)
  const [selectedIds, setSelectedIds] = useState<Set<string>>(new Set())
  const [exporting, setExporting] = useState(false)
  const [notice, setNotice] = useState('')

  const startUnixMs = startTime ? new Date(startTime).getTime() : undefined
  const endUnixMs = endTime ? new Date(endTime).getTime() : undefined
  const timeRangeInvalid = startUnixMs !== undefined && endUnixMs !== undefined && startUnixMs > endUnixMs

  const load = useCallback(async () => {
    if (!appName || timeRangeInvalid) return
    try {
      const result = await fetchRecords(appName, 500, { startUnixMs, endUnixMs })
      setRecords(result.records)
      setSelectedIds(previous => new Set([...previous].filter(id => result.records.some(record => record.id === id))))
      setStats({ count: result.count, total: result.total_bytes, cap: result.cap_bytes })
      setFilteredCount(result.filtered_count ?? result.count)
      setError('')
    } catch {
      setError('读取告警记录失败')
    } finally {
      setLoading(false)
    }
  }, [appName, endUnixMs, startUnixMs, timeRangeInvalid])

  useEffect(() => {
    load()
    const timer = setInterval(load, 4000)
    return () => clearInterval(timer)
  }, [load])

  useEffect(() => { setSelectedIds(new Set()); setNotice('') }, [appName, filter, startTime, endTime])

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

  const toggleSelection = (id: string) => setSelectedIds(previous => {
    const next = new Set(previous)
    if (next.has(id)) next.delete(id)
    else next.add(id)
    return next
  })

  const clearRecords = async () => {
    const ids = [...selectedIds]
    if (!appName || !confirm(ids.length
      ? `确定清空所选 ${ids.length} 条告警记录及其图片、视频？此操作不可撤销。`
      : `确定清空全部 ${stats.count} 条告警记录及其图片、视频？此操作不可撤销。`)) return
    setClearing(true)
    setNotice('')
    try {
      if (ids.length) {
        const result = await deleteSelectedRecords(appName, ids)
        setSelectedIds(new Set(result.failed_ids))
        setNotice(result.failed_ids.length ? `已清空 ${result.deleted_ids.length} 条，${result.failed_ids.length} 条清空失败，可重试。` : `已清空 ${result.deleted_ids.length} 条所选记录。`)
      } else {
        await deleteAllRecords(appName)
        setSelectedIds(new Set())
        setNotice('已清空全部记录。')
      }
      await load()
    } catch { setNotice('清空失败，请重试。') }
    finally { setClearing(false) }
  }

  const exportImages = async () => {
    if (!appName) return
    const ids = selectedIds.size ? [...selectedIds] : undefined
    setExporting(true)
    setNotice('')
    try {
      // 本地目录选择必须直接来自用户点击，不能等网络请求后才调用。
      const directory = await chooseLocalDirectory()
      const result = await prepareRawImageExport(appName, ids)
      const skipped = result.skipped_count ? `，跳过 ${result.skipped_count} 条没有原图的记录` : ''
      if (directory) {
        let completed = 0
        let failed = 0
        for (const image of result.images) {
          setNotice(`正在导出原图 ${completed + failed + 1} / ${result.image_count}…`)
          try {
            const file = await directory.getFileHandle(image.filename, { create: true })
            await writeResponseToFile(await fetch(rawImageDownloadUrl(appName, image.id)), file)
            completed += 1
          } catch { failed += 1 }
        }
        setNotice(`已保存 ${completed} 张原图到电脑目录「${directory.name}」${skipped}${failed ? `，${failed} 张保存失败，请重试` : ''}。`)
      } else {
        downloadToComputer(rawImageExportUrl(appName, result.download_id), result.filename)
        setNotice(`已开始下载 ${result.image_count} 张原图（ZIP）${skipped}。${localDownloadHint}`)
      }
    } catch (error) {
      if (!isSaveCancelled(error)) setNotice(apiErrorMessage(error))
    } finally { setExporting(false) }
  }

  const chooseTimePreset = (preset: Exclude<TimePreset, 'custom'>) => {
    const now = new Date()
    setTimePreset(preset)
    setEndTime('')
    if (preset === 'all') setStartTime('')
    if (preset === 'today') {
      const today = new Date(now.getTime())
      today.setHours(0, 0, 0, 0)
      setStartTime(toLocalDateTime(today))
    }
    if (preset === '24h') setStartTime(toLocalDateTime(new Date(now.getTime() - 24 * 60 * 60 * 1000)))
    if (preset === '7d') setStartTime(toLocalDateTime(new Date(now.getTime() - 7 * 24 * 60 * 60 * 1000)))
  }

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
        <button className="rec-btn" disabled={exporting || clearing || stats.count === 0}
          onClick={exportImages}>{exporting ? '导出中…' : selectedIds.size ? '导出所选原始图片' : '导出全部原始图片'}</button>
        <button className="rec-btn rec-btn-danger" disabled={clearing || exporting || stats.count === 0}
          onClick={clearRecords}>{clearing ? '清空中…' : selectedIds.size ? '清空所选记录' : '清空全部'}
        </button>
      </div>
    </div>
    <div className="records-selection-bar">
      <label><input type="checkbox" disabled={!shown.length || clearing || exporting}
        checked={shown.length > 0 && shown.every(record => selectedIds.has(record.id))}
        ref={element => { if (element) element.indeterminate = shown.some(record => selectedIds.has(record.id)) && !shown.every(record => selectedIds.has(record.id)) }}
        onChange={event => setSelectedIds(event.target.checked ? new Set(shown.map(record => record.id)) : new Set())} />
        全选</label>
      {selectedIds.size > 0 && <span>已选择 {selectedIds.size} 条</span>}
      {selectedIds.size > 0 && <button className="rec-btn rec-btn-secondary" disabled={clearing || exporting}
        onClick={() => setSelectedIds(new Set())}>取消选择</button>}
      <span className="records-save-hint">原图保存到电脑；不支持目录选择时下载 ZIP</span>
    </div>
    {notice && <div className="records-notice" role="status">{notice}</div>}
    <div className="records-toolbar">
      <div className="records-filters">
        {(['all', 'data', 'image', 'video'] as const).map(item =>
          <button key={item} className={`rec-btn ${filter === item ? 'active' : ''}`}
            onClick={() => setFilter(item)}>
            {{ all: '全部', data: '仅数据', image: '图片', video: '视频' }[item]}
          </button>)}
      </div>
      <div className="records-time-filter">
        <span className="records-time-label">时间</span>
        <div className="records-time-presets">
          {([
            ['all', '全部时间'], ['today', '今天'], ['24h', '近24小时'], ['7d', '近7天'],
          ] as const).map(([value, label]) =>
            <button key={value} className={`rec-btn rec-btn-secondary ${timePreset === value ? 'active' : ''}`}
              onClick={() => chooseTimePreset(value)}>{label}</button>)}
        </div>
        <label className="records-time-input">
          <span>开始</span>
          <input type="datetime-local" value={startTime}
            onChange={event => { setStartTime(event.target.value); setTimePreset('custom') }} />
        </label>
        <span className="records-time-separator">至</span>
        <label className="records-time-input">
          <span>结束</span>
          <input type="datetime-local" value={endTime}
            onChange={event => { setEndTime(event.target.value); setTimePreset('custom') }} />
        </label>
        {(startTime || endTime) && <button className="rec-btn rec-btn-secondary"
          onClick={() => chooseTimePreset('all')}>清除时间</button>}
        <span className={`records-filter-summary ${timeRangeInvalid ? 'err' : ''}`}>
          {timeRangeInvalid ? '开始时间不能晚于结束时间'
            : `显示 ${shown.length} / ${filteredCount} 条`}
        </span>
      </div>
    </div>

    {timeRangeInvalid ? <div className="records-empty err">请调整时间范围后再查询。</div>
      : loading ? <div className="records-empty">加载中…</div>
      : error ? <div className="records-empty err">{error}</div>
        : shown.length === 0 ? <div className="records-empty">当前筛选条件下没有告警记录。</div>
          : <div className="records-grid">{shown.map(record => {
            const failed = Object.entries(record.media_statuses ?? {})
              .filter(([, state]) => state.status === 'failed')
            const visibleDeliveries = record.deliveries.slice(0, 1)
            const mediaLabel = record.has_video ? '视频'
              : record.has_annotated_image || record.has_raw_image ? '图片' : '仅数据'
            return <article key={record.id} className={`rec-card rec-card-clickable${selectedIds.has(record.id) ? ' selected' : ''}`}
              role="button" tabIndex={0} onClick={() => openRecord(record)}
              onKeyDown={event => {
                if (event.target !== event.currentTarget) return
                if (event.key === 'Enter' || event.key === ' ') openRecord(record)
              }}>
              <label className="rec-select" onClick={event => event.stopPropagation()}>
                <input type="checkbox" aria-label={`选择记录 ${record.id}`} checked={selectedIds.has(record.id)}
                  disabled={clearing || exporting} onChange={() => toggleSelection(record.id)} />
              </label>
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
              </div>
                <div className="rec-actions" onClick={event => event.stopPropagation()}>
                  <button className="rec-btn rec-btn-secondary"
                    onClick={() => openDetail(record)}>查看详情</button>{' '}
                  <button className="rec-btn" onClick={async () => {
                    await retryRecord(appName!, record.id); load()
                  }}>重试发送</button>{' '}
                  <button className="rec-btn rec-btn-danger" onClick={async () => {
                    if (confirm('确定删除这条告警记录？')) {
                      await deleteRecord(appName!, record.id); load()
                    }
                  }}>删除</button>
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
                <pre className="record-json-code">{JSON.stringify(detail, null, 2)}</pre>
              </details>
            </div>}
      </div>
    </div>}
  </div>
}
