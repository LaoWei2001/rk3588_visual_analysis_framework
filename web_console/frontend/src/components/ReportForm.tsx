import { useCallback, useEffect, useMemo, useState, type ReactNode } from 'react'
import type { Node } from '@xyflow/react'
import {
  apiErrorMessage, fetchAppLogics, fetchRecords, fetchReportContracts, previewDelivery,
  type EventRecord, type LogicDef, type ReportContract,
} from '../api/client'
import { useEditorStore } from '../store/editorStore'
import NumberField from './NumberField'
import ReportContractEditor from './ReportContractEditor'

type Update = (nodeId: string, patch: Record<string, unknown>) => void
type MediaKind = 'annotated_image' | 'raw_image' | 'video'
type Delivery = {
  id: string
  enabled: boolean
  connection_id: string
  contract_id: string
  contract_label?: string
  contract_revision: string
  media: MediaKind[]
  when?: { event_types?: string[] }
}

const MEDIA_LABELS: Record<MediaKind, string> = {
  annotated_image: '带标注图片',
  raw_image: '原始图片',
  video: '事件视频',
}

function Field({ label, children }: { label: string; children: ReactNode }) {
  return <label className="node-field"><span>{label}</span>{children}</label>
}

const copy = <T,>(value: T): T => JSON.parse(JSON.stringify(value)) as T

export default function ReportForm({ node, onUpdate, channelIds = [], allChannelIds = [] }: {
  node: Node
  onUpdate: Update
  channelIds?: number[]
  allChannelIds?: number[]
}) {
  const data = node.data as Record<string, unknown>
  const appName = useEditorStore(state => state.appName)
  const connections = useEditorStore(state => state.deliveryConnections)
  const [contracts, setContracts] = useState<ReportContract[]>([])
  const [logicDefs, setLogicDefs] = useState<LogicDef[]>([])
  const [preview, setPreview] = useState<Record<string, unknown> | null>(null)
  const [testResult, setTestResult] = useState<Record<string, unknown> | null>(null)
  const [localEvents, setLocalEvents] = useState<EventRecord[]>([])
  const [eventCount, setEventCount] = useState(0)
  const [eventTotalBytes, setEventTotalBytes] = useState(0)
  const [eventId, setEventId] = useState('')
  const [busy, setBusy] = useState(false)
  const [contractEditor, setContractEditor] = useState<ReportContract | null | undefined>(undefined)
  const [editorDirty, setEditorDirty] = useState(false)

  const closeEditor = () => {
    if (editorDirty && !window.confirm('接口模板有未保存的修改，确定关闭吗？')) return
    setEditorDirty(false)
    setContractEditor(undefined)
  }

  const loadCatalog = useCallback(() => {
    if (!appName) return
    return Promise.all([fetchReportContracts(appName), fetchAppLogics(appName)])
      .then(([availableContracts, logics]) => {
        setContracts(availableContracts)
        setLogicDefs([
          ...logics.channel_logics,
          ...logics.global_logics,
        ])
      })
      .catch(() => {
        setContracts([])
        setLogicDefs([])
      })
  }, [appName])

  useEffect(() => { loadCatalog() }, [loadCatalog])

  const refreshEvents = useCallback(() => {
    if (!appName) return
    fetchRecords(appName, 100)
      .then(result => {
        setLocalEvents(result.records)
        setEventCount(result.count)
        setEventTotalBytes(result.total_bytes)
      })
      .catch(() => {
        setLocalEvents([])
        setEventCount(0)
        setEventTotalBytes(0)
      })
  }, [appName])

  useEffect(() => refreshEvents(), [refreshEvents])

  const policy = (data.report_policy && typeof data.report_policy === 'object'
    ? data.report_policy : {}) as Record<string, unknown>
  const stored = Array.isArray(policy.deliveries) ? policy.deliveries as Delivery[] : []
  const delivery: Delivery = stored[0] ?? {
    id: `delivery_${node.id}`,
    enabled: true,
    connection_id: '',
    contract_id: '',
    contract_label: '',
    contract_revision: '',
    media: [],
  }
  const reportEnabled = policy.enabled !== false && delivery.enabled !== false
  const logic = logicDefs.find(item => item.name === String(data.logic_name ?? ''))
  const declaredFields = useMemo(() => logic?.report_fields ?? [], [logic])
  const declaredEventTypes = useMemo(() => logic?.event_types ?? [], [logic])
  const declaredEventTypeIds = new Set(declaredEventTypes.map(item => item.id))
  const declaredTemplateIds = new Set(logic?.report_template_ids ?? [])
  const hasExactTemplateCatalog = Array.isArray(logic?.report_template_ids)
  const compatibleContracts = contracts.filter(contract => {
    const belongsToLogicFolder = hasExactTemplateCatalog
      ? declaredTemplateIds.has(contract.id)
      : contract.package_template === true && contract.owner_logic === logic?.name
    return belongsToLogicFolder && (contract.event_types.length === 0
      || contract.event_types.some(eventType => declaredEventTypeIds.has(eventType)))
  })
  const selectedContract = compatibleContracts.find(item => item.id === delivery.contract_id)
  const selectedConnection = connections[delivery.connection_id]
  const compatibleConnections = Object.entries(connections).filter(([, connection]) =>
    selectedContract && connection.adapter === selectedContract.adapter)
  const configuredMediaChannel = Number(data.media_source_channel_id ?? -1)
  const selectedMediaChannel = allChannelIds.includes(configuredMediaChannel)
    ? configuredMediaChannel : undefined
  const isGlobalReport = data.logic_kind === 'global'
  const hasImageMedia = delivery.media.includes('annotated_image') || delivery.media.includes('raw_image')
  const hasVideoMedia = delivery.media.includes('video')

  const setPolicy = (patch: Record<string, unknown>) =>
    onUpdate(node.id, { report_policy: { ...policy, ...patch } })
  const patchDelivery = (patch: Partial<Delivery>) => {
    const next = { ...delivery, ...patch }
    setPolicy({
      ...(patch.enabled === undefined ? {} : { enabled: next.enabled }),
      deliveries: [next],
    })
  }

  useEffect(() => {
    if (!selectedContract || delivery.contract_revision === selectedContract.revision) return
    patchDelivery({
      contract_label: selectedContract.label,
      contract_revision: selectedContract.revision,
      media: copy(selectedContract.media),
    })
    setPreview(null)
    setTestResult(null)
  }, [selectedContract?.id, selectedContract?.revision, delivery.contract_revision])

  const selectConnection = (connectionId: string) => patchDelivery({ connection_id: connectionId })

  const applySelectedContract = (contractId: string) => {
    const contract = contracts.find(item => item.id === contractId)
    if (!contract) {
      patchDelivery({ contract_id: '', contract_label: '', contract_revision: '', connection_id: '', media: [] })
      return
    }
    const keepConnection = connections[delivery.connection_id]?.adapter === contract.adapter
    patchDelivery({
      contract_id: contract.id,
      contract_label: contract.label,
      contract_revision: contract.revision,
      connection_id: keepConnection ? delivery.connection_id : '',
      media: copy(contract.media),
    })
    setPreview(null)
    setTestResult(null)
  }

  const runPreview = async (send: boolean) => {
    setBusy(true)
    try {
      const result = await previewDelivery(
        appName, delivery as unknown as Record<string, unknown>, eventId, send,
      )
      setPreview(result.preview)
      setTestResult(result.test ?? null)
      refreshEvents()
    } catch (error) {
      setTestResult({ ok: false, detail: apiErrorMessage(error) })
    } finally {
      setBusy(false)
    }
  }

  const eventTypes = delivery.when?.event_types ?? []
  const invalidEventTypes = eventTypes.filter(item => !declaredEventTypeIds.has(item))
  const selectedEventTypes = eventTypes.filter(item => declaredEventTypeIds.has(item))
  const setEventTypes = (next: string[]) => patchDelivery({
    when: next.length > 0 ? { event_types: next } : undefined,
  })
  const toggleEventType = (eventType: string, checked: boolean) => {
    const next = checked
      ? [...new Set([...selectedEventTypes, eventType])]
      : selectedEventTypes.filter(item => item !== eventType)
    setEventTypes(next)
  }
  const templateReady = Boolean(selectedConnection && selectedContract && delivery.contract_revision)
  const contractSaved = (saved: ReportContract) => {
    setContracts(current => {
      const exists = current.some(item => item.id === saved.id)
      return exists
        ? current.map(item => item.id === saved.id ? saved : item)
        : [...current, saved].sort((left, right) => left.id.localeCompare(right.id))
    })
    patchDelivery({
      contract_id: saved.id,
      contract_label: saved.label,
      contract_revision: saved.revision,
      connection_id: connections[delivery.connection_id]?.adapter === saved.adapter
        ? delivery.connection_id : '',
      media: copy(saved.media),
    })
    setPreview(null)
    setTestResult({ ok: true, detail: '接口模板已保存并应用。' })
  }

  return <div className="ncp-form">
    <label className="node-toggle ncp-top-toggle">
      <input type="checkbox" checked={reportEnabled}
        onChange={event => patchDelivery({ enabled: event.target.checked })} />
      启用上报
    </label>
    <div className="report-delivery-card">
      {isGlobalReport && hasImageMedia && <div className="report-mapping-help">
        {channelIds.length
          ? `图片来源：${channelIds.map(channelId => `通道 ${channelId}`).join('、')}`
          : '图片来源由事件指定。'}
      </div>}
      {isGlobalReport && hasVideoMedia && <Field label="事件视频来源通道">
        <select
          value={selectedMediaChannel == null ? '' : String(selectedMediaChannel)}
          onChange={event => onUpdate(node.id, {
            media_source_channel_id: event.target.value === '' ? undefined : Number(event.target.value),
          })}>
          <option value="">请选择事件视频来源通道</option>
          {allChannelIds.map(channelId =>
            <option key={channelId} value={channelId}>通道 {channelId}</option>)}
        </select>
        <div className="report-mapping-help">
          仅预录所选通道。
        </div>
      </Field>}
      <Field label="接口模板">
        <select value={delivery.contract_id}
          onFocus={() => { void loadCatalog() }}
          onChange={event => applySelectedContract(event.target.value)}>
          <option value="">请选择接口模板</option>
          {compatibleContracts.map(contract =>
            <option key={contract.id} value={contract.id}>
              {contract.label}
            </option>)}
        </select>
        {logic && compatibleContracts.length === 0 && <div className="report-contract-error">
          当前逻辑函数文件夹没有声明可用的上报模板 JSON。
        </div>}
      </Field>

      <Field label="投递连接">
        <select value={delivery.connection_id} disabled={!selectedContract}
          onChange={event => selectConnection(event.target.value)}>
          <option value="">请选择投递连接</option>
          {compatibleConnections.map(([id, connection]) =>
            <option key={id} value={id}>{id} · {connection.adapter}</option>)}
        </select>
      </Field>

      <div className="report-event-actions">
        <button type="button" className="report-event-button" disabled={!selectedContract}
          onClick={() => setContractEditor(selectedContract ?? null)}>编辑当前模板</button>
      </div>

      {contractEditor !== undefined && Object.keys(connections).length > 0 &&
        <div className="report-contract-overlay">
          <div className="report-contract-modal">
            <button type="button" className="report-contract-close" onClick={closeEditor}
              title="关闭">×</button>
            <ReportContractEditor
              appName={appName}
              adapter={selectedContract?.adapter ?? Object.values(connections)[0]?.adapter ?? 'http'}
              contract={contractEditor}
              logicName={logic?.name ?? ''}
              eventTypes={declaredEventTypes.map(item => item.id)}
              reportFields={declaredFields}
              connection={selectedConnection}
              deliveryId={delivery.id}
              onSaved={contractSaved}
              onDirtyChange={setEditorDirty}
            />
          </div>
        </div>}

      <div className="report-section-title">接收的事件类型</div>
      {logic?.event_types === undefined ? (
        <div className="report-contract-error">
          当前程序包的逻辑目录没有 event_types 元数据，请重新构建部署该程序包后再配置事件过滤。
        </div>
      ) : declaredEventTypes.length === 0 ? (
        <div className="report-contract-error">
          当前算法声明为“不产生上报事件”，不应连接上报节点。
        </div>
      ) : (
        <div className="report-event-type-picker">
          <label className={eventTypes.length === 0 ? 'selected' : ''}>
            <input type="radio" name={`report-event-type-${node.id}`}
              checked={eventTypes.length === 0} onChange={() => setEventTypes([])} />
            <span><strong>全部事件</strong></span>
          </label>
          {declaredEventTypes.map(item => (
            <label key={item.id} className={selectedEventTypes.includes(item.id) ? 'selected' : ''}>
              <input type="checkbox" checked={selectedEventTypes.includes(item.id)}
                onChange={event => toggleEventType(item.id, event.target.checked)} />
              <span>
                <strong>{item.label || item.id}</strong>
                <code>{item.id}</code>
              </span>
            </label>
          ))}
        </div>
      )}
      {invalidEventTypes.length > 0 && (
        <div className="report-contract-error">
          配置中存在当前算法未声明的事件类型：{invalidEventTypes.join('、')}。
          <button type="button" onClick={() => setEventTypes(selectedEventTypes)}>
            清除失效值
          </button>
        </div>
      )}

      {selectedContract && <>
        <div className="report-section-title">模板定义的媒体</div>
        <div className="report-event-actions">
          {selectedContract.media.length
            ? selectedContract.media.map(kind => <span key={kind}>{MEDIA_LABELS[kind]}</span>)
            : <span>仅事件数据</span>}
        </div>

        <details className="report-extra-inputs">
          <summary>查看当前模板字段对接</summary>
          {selectedContract.mapping.map((item, index) =>
            <div key={`${item.target}-${index}`} className="report-map-preview">
              {item.source === 'constant'
                ? `常量 ${JSON.stringify(item.value)}`
                : item.source}
              {' → '}{item.target}
              {item.transform ? ` · ${item.transform}` : ''}
              {item.required ? ' · 必填' : ''}
            </div>)}
          {declaredFields.length > 0 && <div className="report-mapping-help">
            可用算法字段：{declaredFields.map(field => field.key).join('、')}
          </div>}
        </details>
      </>}
    </div>

    {delivery.media.includes('annotated_image') &&
      <div className="report-advanced-section">
        <Field label="带标注图片的叠加内容">
          <select value={String(policy.image_overlay ?? 'custom')}
            onChange={event => setPolicy({ image_overlay: event.target.value })}>
            <option value="none">不叠加</option>
            <option value="custom">与实时画面一致</option>
          </select>
        </Field>
      </div>}
    {delivery.media.includes('video') && <div className="report-advanced-section">
      <Field label="事件前时长（秒）">
        <NumberField min={0} max={120} step={0.5} def={3} value={policy.video_pre_sec ?? 3}
          onChange={value => setPolicy({ video_pre_sec: value ?? 3 })} />
      </Field>
      <Field label="事件后时长（秒）">
        <NumberField min={0} max={120} step={0.5} def={3} value={policy.video_post_sec ?? 3}
          onChange={value => setPolicy({ video_post_sec: value ?? 3 })} />
      </Field>
      <Field label="录像帧率">
        <NumberField min={1} max={30} def={15} value={policy.video_fps ?? 15}
          onChange={value => setPolicy({ video_fps: value ?? 15 })} />
      </Field>
    </div>}

    <div className="report-advanced-section">
      <Field label="事件合并窗口（秒，0=不合并）">
        <NumberField min={0} max={60} step={0.5} def={5} value={policy.merge_window_sec ?? 5}
          onChange={value => setPolicy({ merge_window_sec: value ?? 5 })} />
      </Field>
    </div>

    <div className="report-advanced-section">
      <div className="report-section-title">请求预览与测试发送</div>
      <Field label="预览数据">
        <select value={eventId} onChange={event => setEventId(event.target.value)}>
          <option value="">示例事件</option>
          {localEvents.map(event => <option key={event.id} value={event.id}>
            {event.id} · {event.event_type || 'unknown'} · 通道 {event.channel_id ?? '-'}
          </option>)}
        </select>
      </Field>
      <div className="report-event-actions">
        <span>{eventCount > 0 ? `共 ${eventCount} 条记录` : '暂无记录'}</span>
        <button type="button" className="report-event-button" onClick={refreshEvents}>刷新本地事件</button>
        <button type="button" className="report-event-button"
          disabled={busy || !templateReady} onClick={() => runPreview(false)}>
          预览请求
        </button>
        <button type="button" className="report-event-button"
          disabled={busy || !templateReady || !eventId || !reportEnabled}
          title={!reportEnabled ? '请先开启上报' : !eventId ? '请先选择一条真实本地事件' : ''}
          onClick={() => runPreview(true)}>测试发送</button>
      </div>
      {preview && <pre style={{ whiteSpace: 'pre-wrap', fontSize: 11 }}>{JSON.stringify(preview, null, 2)}</pre>}
      {testResult && <pre style={{ whiteSpace: 'pre-wrap', fontSize: 11 }}>{JSON.stringify(testResult, null, 2)}</pre>}
    </div>
  </div>
}
