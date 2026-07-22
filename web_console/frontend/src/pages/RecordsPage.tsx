import { useEffect, useState, useCallback } from 'react'
import { useParams, useNavigate } from 'react-router-dom'
import {
  fetchRecords, fetchRecordJson, recordImageUrl, recordVideoUrl,
  retryRecord, deleteRecord, type AlarmRecord, type RecordJsonResponse,
} from '../api/client'
import './RecordsPage.css'

function fmtBytes(n: number): string {
  if (n < 1024) return `${n} B`
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(0)} KB`
  return `${(n / 1024 / 1024).toFixed(1)} MB`
}

function fmtTime(value: string | number): string {
  if (typeof value === 'string') return value
  if (!value) return '—'
  return new Date(value).toLocaleString()
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
  const [detailRecord, setDetailRecord] = useState<AlarmRecord | null>(null)
  const [detailJson, setDetailJson] = useState<RecordJsonResponse | null>(null)
  const [detailPayloadIndex, setDetailPayloadIndex] = useState(0)
  const [detailLoading, setDetailLoading] = useState(false)
  const [detailError, setDetailError] = useState('')
  const [recordFilter, setRecordFilter] = useState<
    'all' | 'normal' | 'violation' | 'json' | 'image' | 'video'
  >('all')

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
      setErr('读取待上报记录失败')
    } finally {
      setLoading(false)
    }
  }, [appName])

  const shown = records.filter(r => recordFilter === 'all'
    || (recordFilter === 'normal' && r.record_kind === 'normal')
    || (recordFilter === 'violation' && r.record_kind === 'violation')
    || (recordFilter === 'json' && r.media_mode === 'json')
    || (recordFilter === 'image' && (r.has_image || r.expects_image))
    || (recordFilter === 'video' && (r.has_video || r.expects_video)))

  const openDetail = useCallback(async (record: AlarmRecord) => {
    if (!appName) return
    setDetailRecord(record)
    setDetailJson(null)
    setDetailPayloadIndex(0)
    setDetailError('')
    setDetailLoading(true)
    try {
      setDetailJson(await fetchRecordJson(appName, record.id))
    } catch {
      setDetailError('读取上报 JSON 失败，记录可能已经成功投递并从发件箱移除。')
    } finally {
      setDetailLoading(false)
    }
  }, [appName])

  const closeDetail = () => {
    setDetailRecord(null)
    setDetailJson(null)
    setDetailPayloadIndex(0)
    setDetailError('')
  }

  const selectedPayload = detailJson?.payloads[detailPayloadIndex]
  const displayedEventJson = selectedPayload?.event_json ?? detailJson?.event_json ?? null
  const displayedVariable = selectedPayload?.event_variable ?? detailJson?.event_variable ?? ''

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
        <span className="records-title">{appName} — 待上报记录</span>
        <span className="records-stat">
          {count} 条 · {fmtBytes(totalBytes)}{capBytes ? ` / ${fmtBytes(capBytes)}` : ''}
        </span>
        <button className="rec-btn" style={{ marginLeft: 'auto' }} onClick={load}>↻ 刷新</button>
      </div>

      <div className="records-tip">
        <button className="rec-btn" onClick={() => setRecordFilter('all')}>全部</button>{' '}
        <button className="rec-btn" onClick={() => setRecordFilter('normal')}>正常工序</button>{' '}
        <button className="rec-btn" onClick={() => setRecordFilter('violation')}>违规工序</button>{' '}
        <button className="rec-btn" onClick={() => setRecordFilter('json')}>仅 JSON</button>{' '}
        <button className="rec-btn" onClick={() => setRecordFilter('image')}>图片</button>{' '}
        <button className="rec-btn" onClick={() => setRecordFilter('video')}>视频</button>
      </div>

      <div className="records-tip">
        这里只显示<b>尚未成功投递</b>的业务记录。正常工序为仅 JSON 记录，违规工序可包含图片或视频；
        上传通道恢复并投递成功后，记录会自动从这里消失。
      </div>

      {loading ? (
        <div className="records-empty">加载中…</div>
      ) : err ? (
        <div className="records-empty err">{err}</div>
      ) : records.length === 0 ? (
        <div className="records-empty">暂无待上报记录，当前发件箱为空。</div>
      ) : shown.length === 0 ? (
        <div className="records-empty">当前筛选条件下没有待上报记录。</div>
      ) : (
        <div className="records-grid">
          {shown.map(r => {
            const expectsImage = r.expects_image || r.deliveries?.some(d => d.media === 'image')
            const expectsVideo = r.expects_video || r.deliveries?.some(d => d.media === 'video')
            const jsonOnly = r.media_mode === 'json' || r.deliveries?.some(d => d.media === 'json')
            const mediaLabel = jsonOnly ? '仅 JSON'
              : expectsImage && expectsVideo ? '图片+视频'
                : expectsVideo ? '视频' : expectsImage ? '图片' : '无媒体'
            const mediaClass = jsonOnly ? 'json' : expectsVideo ? 'video' : expectsImage ? 'image' : 'pending'
            const kindLabel = r.record_kind === 'normal' ? '正常工序'
              : r.record_kind === 'violation' ? '违规工序' : '告警事件'
            const deliveryMediaLabel = (media?: string) => media === 'json' ? 'JSON'
              : media === 'image' ? '图片' : media === 'video' ? '视频' : (media || '未知')
            return (
              <div key={r.id} className="rec-card rec-card-clickable"
                   title="点击查看实际上报的业务 JSON" onClick={() => openDetail(r)}>
              {r.has_video ? (
                <video className="rec-thumb" src={recordVideoUrl(appName!, r.id)} controls preload="metadata"
                       onClick={e => e.stopPropagation()} />
              ) : r.has_image ? (
                <a href={recordImageUrl(appName!, r.id, false)} target="_blank" rel="noreferrer"
                   onClick={e => e.stopPropagation()}>
                  <img className="rec-thumb" src={recordImageUrl(appName!, r.id, false)} alt="" loading="lazy" />
                </a>
              ) : jsonOnly ? (
                <div className="rec-thumb records-empty rec-json-thumb">仅 JSON 记录<br />不包含图片或视频</div>
              ) : expectsImage || expectsVideo ? (
                <div className="rec-thumb records-empty">媒体生成中…</div>
              ) : <div className="rec-thumb records-empty">无可用媒体</div>}
              <div className="rec-info">
                <div className="rec-badges">
                  <span className={`rec-badge rec-kind-badge ${r.record_kind}`}>{kindLabel}</span>
                  <span className="rec-badge">{r.state === 'collecting' ? '媒体采集中' : '待投递'}</span>
                  <span className={`rec-badge rec-media-badge ${mediaClass}`}>{mediaLabel}</span>
                </div>
                <span className="rec-line">通道 {r.channel_id ?? '?'} · {r.alarm_type || '—'}</span>
                {(r.trigger_count ?? 1) > 1 && <span className="rec-time">已合并 {r.trigger_count} 次触发</span>}
                <span className="rec-time">{fmtTime(r.snapTime)}</span>
                {r.message && <span className="rec-time">{r.message}</span>}
                {r.deliveries?.map((d, i) => (
                  <span className="rec-time" key={d.id ?? i} title={d.last_error ?? ''}>
                    {deliveryMediaLabel(d.media)} → {d.target}: {d.status}（{d.attempts ?? 0}次）
                    {d.last_error ? ` · ${d.last_error}` : ''}
                  </span>
                ))}
                <span onClick={e => e.stopPropagation()}>
                  <button className="rec-btn" onClick={() => openDetail(r)}>查看 JSON</button>{' '}
                  <button className="rec-btn" onClick={async () => { await retryRecord(appName!, r.id); load() }}>重试</button>{' '}
                  <button className="rec-btn" onClick={async () => {
                    if (confirm('确定删除这条待上报记录？删除后将无法自动补传。')) { await deleteRecord(appName!, r.id); load() }
                  }}>删除</button>
                </span>
              </div>
              </div>
            )
          })}
        </div>
      )}

      {detailRecord && (
        <div className="record-json-backdrop" onClick={closeDetail}>
          <div className="record-json-dialog" onClick={e => e.stopPropagation()}>
            <div className="record-json-header">
              <div>
                <div className="record-json-title">上报业务 JSON</div>
                <div className="record-json-subtitle">{detailRecord.id} · {detailRecord.alarm_type || '—'}</div>
              </div>
              <button className="rec-btn" onClick={closeDetail}>关闭</button>
            </div>
            <div className="record-json-hint">
              {displayedVariable ? <>
                以下内容是上传微服务按当前字段选择与路径映射组装后，实际写入 Dify{' '}
                <code>{displayedVariable}</code> 的 JSON；不包含发件箱内部状态。
              </> : <>
                当前投递没有配置Dify业务JSON变量；以下仅展示可组装的业务JSON，不会作为独立JSON变量发送。
              </>}
            </div>
            {detailJson && detailJson.payloads.length > 1 && (
              <label className="record-json-delivery-select">
                Dify投递
                <select value={detailPayloadIndex} onChange={e => setDetailPayloadIndex(Number(e.target.value))}>
                  {detailJson.payloads.map((payload, index) => (
                    <option key={`${payload.delivery_id}-${index}`} value={index}>
                      {payload.delivery_id || `投递${index + 1}`} · {payload.media || '无媒体'} · {payload.status || '未知状态'}
                    </option>
                  ))}
                </select>
              </label>
            )}
            {detailLoading ? (
              <div className="record-json-state">读取中…</div>
            ) : detailError ? (
              <div className="record-json-state err">{detailError}</div>
            ) : (
              <>
                <pre className="record-json-code">{JSON.stringify(displayedEventJson, null, 2)}</pre>
                {detailJson && <details className="record-json-source">
                  <summary>查看映射前的完整业务JSON</summary>
                  <pre className="record-json-code">{JSON.stringify(detailJson.full_business_json, null, 2)}</pre>
                </details>}
              </>
            )}
          </div>
        </div>
      )}
    </div>
  )
}
