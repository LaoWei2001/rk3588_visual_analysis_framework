import { useEffect, useMemo, useState } from 'react'
import {
  apiErrorMessage, fetchDeliveryAdapters, saveReportContract,
  type DeliveryAdapterDef, type DeliveryConnection, type ReportContract, type ReportField,
} from '../api/client'

type MediaKind = 'annotated_image' | 'raw_image' | 'video'
type Mapping = ReportContract['mapping'][number]

interface Props {
  appName: string
  adapter: string
  contract: ReportContract | null
  logicName: string
  eventTypes: string[]
  reportFields: ReportField[]
  connection?: DeliveryConnection
  deliveryId?: string
  onSaved: (contract: ReportContract) => void
  onDirtyChange?: (dirty: boolean) => void
}

type SourceOption = {
  value: string
  label: string
  group: '系统事件' | '当前算法' | '媒体' | '固定值' | '模板已有来源'
  type: string
}

const MEDIA: Array<{ value: MediaKind; label: string }> = [
  { value: 'annotated_image', label: '带标注图片' },
  { value: 'raw_image', label: '原始图片' },
  { value: 'video', label: '事件视频' },
]

const SYSTEM_SOURCES: SourceOption[] = [
  { value: 'event.id', label: '事件 ID', group: '系统事件', type: 'string' },
  { value: 'event.type', label: '报警类型', group: '系统事件', type: 'string' },
  { value: 'event.message', label: '报警说明', group: '系统事件', type: 'string' },
  { value: 'event.snap_time', label: '抓拍时间', group: '系统事件', type: 'string' },
  { value: 'event.end_time', label: '结束时间', group: '系统事件', type: 'string' },
  { value: 'event.trigger_unix_ms', label: '触发时间戳', group: '系统事件', type: 'number' },
  { value: 'event.trigger_count', label: '事件触发次数', group: '系统事件', type: 'number' },
  { value: 'source.channel_id', label: '通道 ID', group: '系统事件', type: 'number' },
  { value: 'source.video_channel_id', label: '事件视频通道 ID', group: '系统事件', type: 'number' },
  { value: 'event', label: '完整事件对象', group: '系统事件', type: 'json' },
  { value: 'source', label: '完整来源对象', group: '系统事件', type: 'json' },
  { value: 'fields', label: '全部算法字段', group: '系统事件', type: 'json' },
]

const MEDIA_SOURCES: SourceOption[] = [
  { value: 'media.annotated_image', label: '带标注图片', group: '媒体', type: 'file' },
  { value: 'media.raw_image', label: '原始图片', group: '媒体', type: 'file' },
  { value: 'media.video', label: '事件视频', group: '媒体', type: 'file' },
]

const CONSTANT_SOURCE: SourceOption = {
  value: 'constant', label: '固定值', group: '固定值', type: 'string',
}

const newInternalContractId = (logicName: string): string => {
  const owner = logicName.replace(/[^A-Za-z0-9._-]/g, '_') || 'contract'
  const suffix = `${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 10)}`
  return `${owner}.custom_${suffix}`
}

const emptyContract = (adapter: string, logicName: string, eventTypes: string[]): ReportContract => ({
  id: newInternalContractId(logicName),
  version: 1,
  label: '',
  description: '',
  owner_logic: logicName,
  event_types: eventTypes,
  adapter,
  media: [],
  mapping: [],
  revision: '',
  ...(adapter === 'http'
    ? { request: { method: 'POST', path: '/api/events', encoding: 'json' }, success: { http_status: [200] } }
    : {}),
})

const clone = <T,>(value: T): T => JSON.parse(JSON.stringify(value)) as T

const previewFieldValue = (field: ReportField): unknown => {
  if (field.type === 'number') return 3
  if (field.type === 'boolean') return true
  if (field.type === 'json') return { sample: field.label || field.key }
  return `<${field.label || field.key}>`
}

const lookupPath = (root: Record<string, unknown>, path: string): unknown => {
  let current: unknown = root
  for (const part of path.split('.').filter(Boolean)) {
    if (!current || typeof current !== 'object' || Array.isArray(current)
      || !(part in current)) return undefined
    current = (current as Record<string, unknown>)[part]
  }
  return current
}

const setPath = (root: Record<string, unknown>, path: string, value: unknown) => {
  const parts = path.split('.').filter(Boolean)
  if (parts.length === 0) return
  let current = root
  parts.slice(0, -1).forEach(part => {
    if (!current[part] || typeof current[part] !== 'object' || Array.isArray(current[part])) {
      current[part] = {}
    }
    current = current[part] as Record<string, unknown>
  })
  current[parts[parts.length - 1]] = value
}

const previewBasename = (value: unknown): string => {
  const text = String(value ?? '').replace(/\\/g, '/')
  return text.split('/').pop() || text
}

const coercePreview = (value: unknown, type: string): unknown => {
  if (!type || value === null || value === undefined) return value
  if (type === 'string') return String(value)
  if (type === 'number') {
    const number = Number(value)
    if (!Number.isFinite(number)) throw new Error(`${String(value)} 不能转换为数字`)
    return number
  }
  if (type === 'boolean') {
    if (typeof value === 'boolean') return value
    const text = String(value).trim().toLowerCase()
    if (['true', '1', 'yes', 'on'].includes(text)) return true
    if (['false', '0', 'no', 'off'].includes(text)) return false
    throw new Error(`${String(value)} 不能转换为布尔值`)
  }
  if (type === 'json') return typeof value === 'string' ? JSON.parse(value) : value
  return value
}

const transformPreview = (value: unknown, transform: string): unknown => {
  if (!transform) return value
  if (transform === 'json_string') return JSON.stringify(value)
  const filename = previewBasename(value)
  if (transform === 'base64') return `<Base64 ${filename}>`
  if (transform === 'data_url') {
    const mime = filename.endsWith('.mp4') ? 'video/mp4' : 'image/jpeg'
    return `data:${mime};base64,<Base64 ${filename}>`
  }
  if (transform === 'file') return value
  return value
}

const maskHeaders = (headers: Record<string, unknown>): Record<string, unknown> => {
  const secretWords = ['authorization', 'token', 'api-key', 'apikey', 'secret']
  return Object.fromEntries(Object.entries(headers).map(([key, value]) => [
    key,
    secretWords.some(word => key.toLowerCase().includes(word)) ? '***' : value,
  ]))
}

const joinHttpUrl = (baseValue: unknown, pathValue: unknown): string => {
  let base = String(baseValue ?? '').trim()
  if (!base) base = 'http://<连接地址>'
  else if (!base.includes('://')) base = `http://${base}`
  const path = String(pathValue ?? '').trim().replace(/^\/+/, '')
  return `${base.replace(/\/+$/, '')}/${path}`
}

function templateRequestPreview(
  draft: ReportContract,
  reportFields: ReportField[],
  eventTypes: string[],
  connection: DeliveryConnection | undefined,
  deliveryId: string | undefined,
): Record<string, unknown> {
  const now = new Date()
  const snapTime = `${now.getFullYear()}-${String(now.getMonth() + 1).padStart(2, '0')}-${String(now.getDate()).padStart(2, '0')} ${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}:${String(now.getSeconds()).padStart(2, '0')}`
  const fields = Object.fromEntries(reportFields.map(field => [field.key, previewFieldValue(field)]))
  const sourcePool: Record<string, unknown> = {
    event: {
      id: 'preview-event', type: eventTypes[0] || 'sample_event', message: '接口映射预览',
      trigger_unix_ms: now.getTime(), snap_time: snapTime, end_time: snapTime, trigger_count: 1,
    },
    source: { channel_id: 0, video_channel_id: 0, parameters: {} },
    fields,
    media: {
      annotated_image: '/preview/annotated.jpg',
      raw_image: '/preview/raw.jpg',
      video: '/preview/clip.mp4',
    },
  }
  const parts: Record<'body' | 'query' | 'form' | 'header' | 'file', Record<string, unknown>> = {
    body: {}, query: {}, form: {}, header: {}, file: {},
  }
  draft.mapping.forEach(mapping => {
    if (!mapping.source || !mapping.target) return
    let value: unknown
    if (mapping.source === 'constant') {
      const type = inferConstantType(mapping)
      value = parseConstant(mapping.value, type)
    } else {
      value = lookupPath(sourcePool, mapping.source)
      if (value === undefined) value = mapping.required
        ? `<required: ${mapping.source}>` : `<${mapping.source}>`
      value = coercePreview(value, mapping.type ?? '')
    }
    value = transformPreview(value, mapping.transform ?? '')
    setPath(parts[mapping.location ?? 'body'], mapping.target, value)
  })

  if (draft.adapter === 'dify_workflow') {
    Object.entries(parts.file).forEach(([target, path]) => {
      setPath(parts.body, target, { file: previewBasename(path) })
    })
    let base = String(connection?.api_url ?? '').trim() || 'http://<Dify地址>'
    base = base.replace(/\/+$/, '').replace(/\/v1\/(files\/upload|workflows\/run)$/, '')
    return {
      adapter: 'dify_workflow', method: 'POST',
      upload_url: `${base}/v1/files/upload`, workflow_url: `${base}/v1/workflows/run`,
      inputs: parts.body,
    }
  }

  const connectionHeaders = connection?.headers && typeof connection.headers === 'object'
    && !Array.isArray(connection.headers) ? connection.headers as Record<string, unknown> : {}
  const headers = { ...connectionHeaders, ...parts.header }
  if (!Object.keys(headers).some(key => key.toLowerCase() === 'x-idempotency-key')) {
    headers['X-Idempotency-Key'] = `preview-event:${deliveryId || 'delivery-preview'}`
  }
  return {
    adapter: 'http',
    method: String(draft.request?.method ?? 'POST').toUpperCase(),
    url: joinHttpUrl(connection?.base_url, draft.request?.path),
    encoding: String(draft.request?.encoding ?? 'json'),
    json_part: String(draft.request?.json_part ?? '').trim() || null,
    headers: maskHeaders(headers),
    query: parts.query,
    body: parts.body,
    form: parts.form,
    files: Object.fromEntries(Object.entries(parts.file).map(([key, value]) => [key, previewBasename(value)])),
  }
}

function inferConstantType(mapping: Mapping): string {
  if (mapping.type) return mapping.type
  if (typeof mapping.value === 'number') return 'number'
  if (typeof mapping.value === 'boolean') return 'boolean'
  if (mapping.value !== null && typeof mapping.value === 'object') return 'json'
  return 'string'
}

function parseConstant(value: unknown, type: string): unknown {
  const text = String(value ?? '')
  if (type === 'number') {
    const number = Number(text)
    if (!Number.isFinite(number)) throw new Error(`固定值 ${text} 不是有效数字`)
    return number
  }
  if (type === 'boolean') {
    if (text === 'true') return true
    if (text === 'false') return false
    throw new Error('布尔固定值只能填写 true 或 false')
  }
  if (type === 'json') {
    try { return JSON.parse(text) }
    catch { throw new Error('JSON 固定值格式不正确') }
  }
  return text
}

function successStatuses(contract: ReportContract): string {
  const raw = contract.success?.http_status
  return Array.isArray(raw) ? raw.join(',') : '200'
}

export default function ReportContractEditor({
  appName, adapter, contract, logicName, eventTypes, reportFields, onSaved,
  connection, deliveryId, onDirtyChange,
}: Props) {
  const editing = contract !== null
  const [draft, setDraft] = useState<ReportContract>(() =>
    clone(contract ?? emptyContract(adapter, logicName, eventTypes)))
  const [adapters, setAdapters] = useState<DeliveryAdapterDef[]>([])
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState('')
  const [dirty, setDirty] = useState(false)
  const markDirty = () => { if (!dirty) { setDirty(true); onDirtyChange?.(true) } }
  const clearDirty = () => { if (dirty) { setDirty(false); onDirtyChange?.(false) } }

  useEffect(() => {
    setDraft(clone(contract ?? emptyContract(adapter, logicName, eventTypes)))
    setError('')
    clearDirty()
  }, [adapter, contract, logicName, eventTypes.join('|')])

  useEffect(() => {
    fetchDeliveryAdapters(appName).then(setAdapters).catch(() => setAdapters([]))
  }, [appName])

  const currentAdapter = adapters.find(item => item.id === draft.adapter)
  const transforms = currentAdapter?.transforms ?? ['']
  const sources = useMemo<SourceOption[]>(() => {
    const known = [
      ...SYSTEM_SOURCES,
      ...reportFields.map(field => ({
        value: `fields.${field.key}`,
        label: field.label || field.key,
        group: '当前算法' as const,
        type: field.type,
      })),
      ...MEDIA_SOURCES,
      CONSTANT_SOURCE,
    ]
    const knownValues = new Set(known.map(item => item.value))
    const existing = draft.mapping
      .map(item => item.source)
      .filter(source => source && !knownValues.has(source))
      .filter((source, index, all) => all.indexOf(source) === index)
      .map(source => ({
        value: source,
        label: source,
        group: '模板已有来源' as const,
        type: '',
      }))
    return [...known, ...existing]
  }, [draft.mapping, reportFields])

  const patch = (value: Partial<ReportContract>) => {
    markDirty()
    setDraft(current => ({ ...current, ...value }))
  }
  const patchMapping = (index: number, value: Partial<Mapping>) => {
    markDirty()
    setDraft(current => ({
      ...current,
      mapping: current.mapping.map((item, itemIndex) =>
        itemIndex === index ? { ...item, ...value } : item),
    }))
  }
  const changeAdapter = (nextAdapter: string) => {
    markDirty()
    const next = emptyContract(nextAdapter, logicName, eventTypes)
    setDraft(current => ({
      ...current,
      adapter: nextAdapter,
      media: [],
      mapping: [],
      request: next.request,
      success: next.success,
    }))
  }
  const toggleMedia = (kind: MediaKind, checked: boolean) => {
    markDirty()
    setDraft(current => ({
      ...current,
      media: checked
        ? [...new Set([...current.media, kind])]
        : current.media.filter(item => item !== kind),
      mapping: checked
        ? current.mapping
        : current.mapping.filter(item => item.source !== `media.${kind}`),
    }))
  }
  const addMapping = () => {
    markDirty()
    setDraft(current => ({
      ...current,
      mapping: [...current.mapping, {
        source: reportFields.length ? `fields.${reportFields[0].key}` : 'event.type',
        target: '',
        location: 'body',
        required: true,
      }],
    }))
  }
  const removeMapping = (index: number) => {
    markDirty()
    setDraft(current => ({
      ...current,
      mapping: current.mapping.filter((_, itemIndex) => itemIndex !== index),
    }))
  }
  const selectSource = (index: number, source: string) => {
    markDirty()
    const option = sources.find(item => item.value === source)
    const patchValue: Partial<Mapping> = {
      source,
      ...(!draft.mapping[index]?.target && source !== 'constant'
        ? { target: source.split('.').pop() ?? '' }
        : {}),
    }
    if (source === 'constant') {
      patchValue.value = ''
      patchValue.type = 'string'
      patchValue.transform = ''
      patchValue.location = 'body'
    } else {
      delete patchValue.value
      patchValue.type = ''
      if (source.startsWith('media.')) {
        const kind = source.slice('media.'.length) as MediaKind
        if (!draft.media.includes(kind)) {
          setDraft(current => ({ ...current, media: [...current.media, kind] }))
        }
        patchValue.transform = draft.adapter === 'dify_workflow' ? 'file' : 'base64'
        patchValue.location = draft.adapter === 'dify_workflow' ? 'file' : 'body'
        if (draft.adapter === 'dify_workflow') patchValue.file_mode = 'list'
      } else if (option?.type === 'json') {
        patchValue.transform = draft.adapter === 'dify_workflow' ? 'json_string' : ''
      } else {
        patchValue.transform = ''
      }
    }
    patchMapping(index, patchValue)
  }

  const normalizedDraft = (incrementVersion: boolean): ReportContract => {
    const currentId = draft.id.trim()
    const id = /^[A-Za-z0-9._-]+$/.test(currentId)
      ? currentId : newInternalContractId(logicName)
    if (!draft.label.trim()) throw new Error('请填写模板名称')
    if (draft.mapping.length === 0) throw new Error('请至少增加一条字段映射')
    const mapping = draft.mapping.map(item => {
      const result = { ...item }
      if (result.source === 'constant') {
        const type = inferConstantType(result)
        result.type = type
        result.value = parseConstant(result.value, type)
      } else {
        delete result.value
        if (!result.type) delete result.type
      }
      if (!result.transform) delete result.transform
      if (result.transform !== 'file') delete result.file_mode
      return result
    })
    const normalized: ReportContract = {
      ...draft,
      id,
      version: incrementVersion && draft.revision ? draft.version + 1 : Math.max(1, draft.version || 1),
      owner_logic: logicName,
      event_types: eventTypes,
      label: draft.label.trim(),
      description: draft.description?.trim() ?? '',
      mapping,
    }
    if (draft.adapter === 'http') {
      const statuses = successStatuses(draft)
        .split(',').map(item => Number(item.trim())).filter(Number.isFinite)
      if (statuses.length === 0) throw new Error('至少填写一个 HTTP 成功状态码')
      normalized.success = { ...draft.success, http_status: statuses }
    }
    return normalized
  }

  const save = async () => {
    setError('')
    setSaving(true)
    try {
      const normalized = normalizedDraft(true)
      const saved = await saveReportContract(appName, normalized)
      setDraft(clone(saved))
      clearDirty()
      onSaved(saved)
    } catch (reason) {
      setError(apiErrorMessage(reason))
    } finally {
      setSaving(false)
    }
  }

  const exportJson = () => {
    setError('')
    try {
      const normalized = normalizedDraft(false)
      const portable: Partial<ReportContract> = { ...normalized }
      delete portable.id
      delete portable.origin
      delete portable.revision
      delete portable.package_template
      const blob = new Blob([`${JSON.stringify(portable, null, 2)}\n`], {
        type: 'application/json;charset=utf-8',
      })
      const href = URL.createObjectURL(blob)
      const link = document.createElement('a')
      const filename = normalized.label.replace(/[\\/:*?"<>|]/g, '_').trim() || 'report-template'
      link.href = href
      link.download = `${filename}.json`
      document.body.appendChild(link)
      link.click()
      link.remove()
      URL.revokeObjectURL(href)
    } catch (reason) {
      setError(apiErrorMessage(reason))
    }
  }

  const groups: SourceOption['group'][] = [
    '系统事件', '当前算法', '媒体', '固定值', '模板已有来源',
  ]
  const requestPreview = useMemo(() => {
    try {
      return { value: templateRequestPreview(draft, reportFields, eventTypes, connection, deliveryId) }
    } catch (reason) {
      return { error: apiErrorMessage(reason) }
    }
  }, [draft, reportFields, eventTypes, connection, deliveryId])

  return <div className="report-contract-editor">
    <div className="report-section-title">
      {editing ? `编辑模板：${contract?.label}` : '新建接口模板'}
    </div>
    <div className="report-contract-grid">
      <label className="node-field"><span>模板名称</span>
        <input value={draft.label} placeholder="周期截图服务器"
          onChange={event => patch({ label: event.target.value })} />
      </label>
      <label className="node-field"><span>适配器</span>
        <select value={draft.adapter} disabled={editing || Boolean(draft.revision)}
          onChange={event => changeAdapter(event.target.value)}>
          {adapters.map(item => <option key={item.id} value={item.id}>{item.label}</option>)}
        </select>
      </label>
      <label className="node-field report-contract-description"><span>说明</span>
        <input value={draft.description ?? ''} placeholder="这个模板对应哪个服务器或 Dify 工作流"
          onChange={event => patch({ description: event.target.value })} />
      </label>
    </div>

    <div className="report-section-title">需要生成的媒体</div>
    <div className="report-media-picker">
      <label className={draft.media.length === 0 ? 'report-media-only-data' : ''}>
        <input type="checkbox" checked={draft.media.length === 0}
          onChange={event => {
            markDirty()
            if (event.target.checked) {
              setDraft(current => ({ ...current, media: [] }))
            } else {
              setDraft(current => ({ ...current, media: ['annotated_image'] }))
            }
          }} />
        仅数据，不生成媒体
      </label>
      {MEDIA.filter(item => !currentAdapter
        || currentAdapter.supported_media.includes(item.value)).map(item =>
        <label key={item.value} className={draft.media.length === 0 ? 'report-media-disabled' : ''}>
          <input type="checkbox" checked={draft.media.includes(item.value)}
            disabled={draft.media.length === 0}
            onChange={event => toggleMedia(item.value, event.target.checked)} />
          {item.label}
        </label>)}
    </div>

    {draft.adapter === 'http' && <div className="report-contract-http">
      <label className="node-field"><span>HTTP 方法</span>
        <select value={String(draft.request?.method ?? 'POST')}
          onChange={event => patch({ request: { ...draft.request, method: event.target.value } })}>
          <option value="POST">POST</option>
          <option value="PUT">PUT</option>
          <option value="PATCH">PATCH</option>
        </select>
      </label>
      <label className="node-field"><span>接口路径</span>
        <input value={String(draft.request?.path ?? '')} placeholder="/api/events"
          onChange={event => patch({ request: { ...draft.request, path: event.target.value } })} />
      </label>
      <label className="node-field"><span>请求编码</span>
        <select value={String(draft.request?.encoding ?? 'json')}
          onChange={event => patch({ request: { ...draft.request, encoding: event.target.value } })}>
          <option value="json">JSON</option>
          <option value="form">Form</option>
          <option value="multipart">Multipart</option>
        </select>
      </label>
      {draft.request?.encoding === 'multipart' &&
        <label className="node-field"><span>JSON Part 名称（存在 body 字段时必填）</span>
          <input value={String(draft.request?.json_part ?? '')} placeholder="payload"
            onChange={event => patch({
              request: { ...draft.request, json_part: event.target.value },
            })} />
        </label>}
      <label className="node-field"><span>成功状态码（逗号分隔）</span>
        <input value={successStatuses(draft)}
          onChange={event => patch({
            success: { ...draft.success, http_status: event.target.value.split(',') },
          })} />
      </label>
      <label className="node-field"><span>响应体校验（可选）</span>
        <div className="report-success-check">
          <input value={String(draft.success?.json_path ?? '')} placeholder="路径如 code"
            style={{ flex: 1 }}
            onChange={event => patch({
              success: { ...draft.success, json_path: event.target.value },
            })} />
          <span>==</span>
          <input value={draft.success?.equals === undefined
            ? '' : JSON.stringify(draft.success.equals)}
            placeholder="200" style={{ flex: 1 }}
            onChange={event => {
              const text = event.target.value
              let value: unknown = text
              try { value = text === '' ? undefined : JSON.parse(text) } catch { /* 保留文本 */ }
              patch({ success: { ...draft.success, equals: value } })
            }} />
        </div>
      </label>
    </div>}

    <div className="report-section-title">最终 JSON / Dify inputs 字段</div>
    {draft.mapping.map((item, index) => {
      const constantType = inferConstantType(item)
      return <div className="report-mapping-row" key={index}>
        <div className="report-map-main">
          <div className="report-map-field">
            <label>数据来源</label>
            <select value={item.source} onChange={event => selectSource(index, event.target.value)}>
              {groups.map(group => {
                const options = sources.filter(option => option.group === group)
                if (options.length === 0) return null
                return <optgroup key={group} label={group}>
                  {options.map(option =>
                    <option key={option.value} value={option.value}>
                      {option.label} · {option.value}
                    </option>)}
                </optgroup>
              })}
            </select>
          </div>
          <div className="report-map-field">
            <label>远端字段名（支持点路径）</label>
            <input value={item.target} placeholder="alarmData.score"
              onChange={event => patchMapping(index, { target: event.target.value })} />
          </div>
          <button type="button" className="report-row-delete" title="删除字段"
            onClick={() => removeMapping(index)}>×</button>
        </div>

        {item.source === 'constant' && <div className="report-constant-grid">
          <label className="node-field"><span>固定值</span>
            <input value={typeof item.value === 'object'
              ? JSON.stringify(item.value) : String(item.value ?? '')}
              onChange={event => patchMapping(index, { value: event.target.value })} />
          </label>
          <label className="node-field"><span>固定值类型</span>
            <select value={constantType}
              onChange={event => patchMapping(index, { type: event.target.value })}>
              <option value="string">字符串</option>
              <option value="number">数字</option>
              <option value="boolean">布尔值</option>
              <option value="json">JSON</option>
            </select>
          </label>
        </div>}

        <div className="report-map-advanced-body">
          <label className="node-field"><span>请求位置</span>
            <select value={item.location ?? 'body'}
              onChange={event => patchMapping(index, {
                location: event.target.value as Mapping['location'],
                ...(event.target.value === 'file' ? { transform: 'file', file_mode: 'list' as const } : {}),
              })}>
              <option value="body">JSON / Dify input</option>
              {draft.adapter === 'http' && <option value="query">Query</option>}
              {draft.adapter === 'http' && <option value="form">Form</option>}
              {draft.adapter === 'http' && <option value="header">Header</option>}
              <option value="file">文件</option>
            </select>
          </label>
          {item.source !== 'constant' && <label className="node-field"><span>类型转换</span>
            <select value={item.type ?? ''}
              onChange={event => patchMapping(index, { type: event.target.value })}>
              <option value="">保持原类型</option>
              <option value="string">字符串</option>
              <option value="number">数字</option>
              <option value="boolean">布尔值</option>
              <option value="json">JSON</option>
            </select>
          </label>}
          <label className="node-field"><span>内容转换</span>
            <select value={item.transform ?? ''}
              onChange={event => patchMapping(index, {
                transform: event.target.value,
                ...(event.target.value === 'file'
                  ? { file_mode: 'list' as const, location: 'file' as const }
                  : item.location === 'file' ? { location: 'body' as const } : {}),
              })}>
              {transforms.map(transform => <option key={transform || 'none'} value={transform}>
                {transform || '无'}
              </option>)}
            </select>
          </label>
          <label className="report-required">
            <input type="checkbox" checked={item.required === true}
              onChange={event => patchMapping(index, { required: event.target.checked })} />
            缺少该变量时判定投递无效
          </label>
        </div>
        <div className="report-map-preview">
          {item.source === 'constant' ? `常量 ${JSON.stringify(item.value ?? '')}` : item.source}
          {' → '}{item.location ?? 'body'}:{item.target || '（请填写远端字段）'}
        </div>
      </div>
    })}

    <button type="button" className="report-add-mapping" onClick={addMapping}>＋增加字段</button>
    {reportFields.length === 0 && <div className="report-mapping-help">
      当前算法没有声明 report_fields；仍可选择系统字段、完整 fields 对象、媒体和固定值。
    </div>}

    <div className="report-section-title">模板请求实时预览</div>
    {requestPreview.error
      ? <div className="report-contract-error">预览失败：{requestPreview.error}</div>
      : <pre className="report-request-preview">{JSON.stringify(requestPreview.value, null, 2)}</pre>}

    {error && <div className="report-contract-error">{error}</div>}
    <div className="report-contract-actions">
      <button type="button" disabled={saving} onClick={exportJson}>导出 JSON</button>
      <button type="button" disabled={saving} onClick={save}>
        {saving ? '保存中…' : '保存接口模板'}
      </button>
    </div>
  </div>
}
