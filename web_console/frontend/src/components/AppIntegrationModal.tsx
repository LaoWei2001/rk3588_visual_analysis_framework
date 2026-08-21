import { useEffect, useState, type ReactNode } from 'react'
import {
  apiErrorMessage, deleteReportContract, fetchConnections, fetchDeliveryAdapters, fetchOtaConfig,
  fetchReportContracts, saveConnections, saveOtaConfig,
  type AdapterConnectionField, type DeliveryAdapterDef, type DeliveryConnection,
  type DeliveryConnectionsConfig, type OtaConfig, type ReportContract,
} from '../api/client'
import NumberField from './NumberField'
import { useEditorStore } from '../store/editorStore'
import './AppIntegrationModal.css'

interface Props {
  appName: string
  onClose: () => void
  onToast: (message: string, ok?: boolean) => void
}

const EMPTY_CONNECTIONS: DeliveryConnectionsConfig = { connections: {} }
const EMPTY_OTA: OtaConfig = { platform_ws_host: '' }
const inputStyle: React.CSSProperties = {
  background: '#0f1117', color: '#e6e9ef', border: '1px solid #2e3352',
  borderRadius: 6, padding: '6px 8px', fontSize: 13, width: '100%',
}
const sectionTitle: React.CSSProperties = {
  fontSize: 13, fontWeight: 600, color: '#7aa2f7', margin: '18px 0 8px',
  borderBottom: '1px solid #2e3352', paddingBottom: 6,
}

function Field({ label, children }: { label: string; children: ReactNode }) {
  return <label style={{ display: 'flex', flexDirection: 'column', gap: 4, marginBottom: 10 }}>
    <span style={{ fontSize: 12, color: '#9aa4b2' }}>{label}</span>{children}
  </label>
}

function ConnectionField({ definition, value, onChange, onError, onEdit }: {
  definition: AdapterConnectionField
  value: unknown
  onChange: (value: unknown) => void
  onError: (message: string) => void
  onEdit: () => void
}) {
  const actual = value ?? definition.default ?? ''
  const [jsonText, setJsonText] = useState(() => JSON.stringify(actual || {}, null, 2))
  useEffect(() => {
    if (definition.type === 'json') setJsonText(JSON.stringify(actual || {}, null, 2))
  }, [actual, definition.type])
  if (definition.type === 'number') {
    return <NumberField style={inputStyle} def={Number(definition.default ?? 0)}
      value={typeof actual === 'number' ? actual : Number(actual)}
      onChange={next => { onEdit(); onChange(next ?? definition.default ?? 0) }} />
  }
  if (definition.type === 'select') {
    return <select style={inputStyle} value={String(actual)}
      onChange={event => { onEdit(); onChange(event.target.value) }}>
      {(definition.options ?? []).map(option => <option key={option}>{option}</option>)}
    </select>
  }
  if (definition.type === 'json') {
    return <textarea style={{ ...inputStyle, minHeight: 86, fontFamily: 'monospace' }}
      value={jsonText} onChange={event => { setJsonText(event.target.value); onEdit() }}
      onBlur={() => {
        try { onChange(JSON.parse(jsonText)) }
        catch { onError(`${definition.label} 必须是有效 JSON`) }
      }} />
  }
  return <input style={inputStyle} type={definition.type === 'secret' ? 'password' : 'text'}
    value={String(actual)} placeholder={definition.required ? '必填' : ''}
    onChange={event => { onEdit(); onChange(event.target.value) }} />
}

export default function AppIntegrationModal({ appName, onClose, onToast }: Props) {
  const [config, setConfig] = useState<DeliveryConnectionsConfig>(EMPTY_CONNECTIONS)
  const [adapters, setAdapters] = useState<DeliveryAdapterDef[]>([])
  const [contracts, setContracts] = useState<ReportContract[]>([])
  const [ota, setOta] = useState<OtaConfig>(EMPTY_OTA)
  const [loading, setLoading] = useState(true)
  const [saving, setSaving] = useState(false)
  const setConnections = useEditorStore(state => state.setDeliveryConnections)
  const dirty = useEditorStore(state => state.appIntegrationDirty)
  const setDirty = useEditorStore(state => state.setAppIntegrationDirty)
  const markDirty = () => setDirty(true)

  useEffect(() => {
    let alive = true
    Promise.all([
      fetchConnections(appName), fetchDeliveryAdapters(appName),
      fetchReportContracts(appName), fetchOtaConfig(appName),
    ]).then(([connections, adapterCatalog, contractCatalog, otaConfig]) => {
      if (!alive) return
      setConfig({ connections: connections.connections ?? {} })
      setAdapters(adapterCatalog)
      setContracts(contractCatalog)
      setOta({ ...EMPTY_OTA, ...otaConfig })
      setDirty(false)
    }).catch(error => onToast(`加载 ${appName} 应用集成失败: ${apiErrorMessage(error)}`, false))
      .finally(() => { if (alive) setLoading(false) })
    return () => { alive = false; setDirty(false) }
  }, [appName, onToast, setDirty])

  const patchConnection = (id: string, patch: Partial<DeliveryConnection>) => {
    markDirty()
    setConfig(current => ({ connections: {
      ...current.connections,
      [id]: { ...current.connections[id], ...patch } as DeliveryConnection,
    } }))
  }
  const addConnection = () => {
    if (!adapters.length) return
    let index = 1
    while (config.connections[`connection_${index}`]) index++
    const adapter = adapters[0]
    const defaults = Object.fromEntries(adapter.connection_fields
      .filter(field => field.default !== undefined).map(field => [field.key, field.default]))
    markDirty()
    setConfig(current => ({ connections: {
      ...current.connections,
      [`connection_${index}`]: { adapter: adapter.id, ...defaults },
    } }))
  }
  const removeConnection = (id: string) => {
    markDirty()
    setConfig(current => {
      const connections = { ...current.connections }
      delete connections[id]
      return { connections }
    })
  }
  const renameConnection = (oldId: string, rawId: string) => {
    const id = rawId.trim()
    if (!id || id === oldId) return
    if (!/^[A-Za-z0-9._-]+$/.test(id) || config.connections[id]) {
      onToast('连接 ID 必须唯一，只能包含字母、数字、点、下划线和短横线', false)
      return
    }
    markDirty()
    setConfig(current => {
      const connections = { ...current.connections, [id]: current.connections[oldId] }
      delete connections[oldId]
      return { connections }
    })
  }
  const changeAdapter = (id: string, adapterId: string) => {
    const adapter = adapters.find(item => item.id === adapterId)
    if (!adapter) return
    const defaults = Object.fromEntries(adapter.connection_fields
      .filter(field => field.default !== undefined).map(field => [field.key, field.default]))
    markDirty()
    setConfig(current => ({ connections: {
      ...current.connections, [id]: { adapter: adapterId, ...defaults },
    } }))
  }
  const save = async () => {
    setSaving(true)
    try {
      await Promise.all([saveConnections(appName, config), saveOtaConfig(appName, ota)])
      setConnections(config.connections)
      setDirty(false)
      onToast(`已保存 ${appName} 的应用集成配置`)
    } catch (error) {
      onToast(`保存失败: ${apiErrorMessage(error)}`, false)
    } finally { setSaving(false) }
  }
  const close = () => {
    if (dirty && !window.confirm(`${appName} 的应用集成有未保存改动，确定关闭？`)) return
    setDirty(false)
    onClose()
  }

  // 不绑定遮罩点击关闭：文本选择、粘贴或误点页面空白都不应丢失正在编辑的连接配置。
  return <div className="app-integration-overlay">
    <div className="app-integration-dialog" role="dialog" aria-modal="true" aria-label={`应用集成：${appName}`}>
    <div className="app-integration-header">
      <div>
        <div className="app-integration-title">应用集成</div>
        <div className="app-integration-app">当前程序：{appName}</div>
      </div>
      <button type="button" className="app-integration-close" onClick={close}
        title="关闭应用集成" aria-label="关闭应用集成">×</button>
    </div>
    <div className="app-integration-content">
      {loading ? <div style={{ padding: 40, textAlign: 'center' }}>加载中…</div> : <>
        <div style={sectionTitle}>投递连接</div>
        <div style={{ fontSize: 12, color: '#9aa4b2', lineHeight: 1.6, marginBottom: 12 }}>
          这些连接只属于 <strong>{appName}</strong>。契约决定请求格式，连接只保存地址、认证和超时。
        </div>
        {Object.entries(config.connections).map(([id, connection]) => {
          const adapter = adapters.find(item => item.id === connection.adapter)
          return <div key={id} className="app-integration-connection">
            <div className="app-integration-connection-header">
              <input style={inputStyle} defaultValue={id} key={id}
                onBlur={event => renameConnection(id, event.target.value)} />
              <select style={inputStyle} value={connection.adapter}
                onChange={event => changeAdapter(id, event.target.value)}>
                {adapters.map(item => <option key={item.id} value={item.id}>{item.label}</option>)}
              </select>
              <button type="button" className="app-integration-button app-integration-button-danger"
                onClick={() => removeConnection(id)}>删除</button>
            </div>
            {adapter?.connection_fields.map(definition => <Field key={definition.key}
              label={`${definition.label}${definition.required ? ' *' : ''}`}>
              <ConnectionField definition={definition} value={connection[definition.key]}
                onChange={value => patchConnection(id, { [definition.key]: value })}
                onError={message => onToast(message, false)} onEdit={markDirty} />
            </Field>)}
          </div>
        })}
        <button type="button" className="app-integration-button app-integration-button-add"
          onClick={addConnection} disabled={!adapters.length}>＋ 新增投递连接</button>

        <div style={sectionTitle}>当前程序包契约</div>
        {contracts.map(contract => <div key={contract.id} style={{
          display: 'grid', gridTemplateColumns: '1fr auto auto', gap: 10, alignItems: 'center',
          padding: '8px 10px', borderBottom: '1px solid #2e3352', fontSize: 12,
        }}>
          <div><strong>{contract.label}</strong><div style={{ color: '#9aa4b2' }}>
            {contract.id} · {contract.owner_logic || '通用'} · v{contract.version} · {contract.revision.slice(0, 12)}
          </div></div>
          <span>{contract.origin === 'custom' ? '应用自定义' : '程序包模板'}</span>
          {contract.origin === 'custom' && <button type="button"
            className="app-integration-button app-integration-button-danger" onClick={async () => {
            if (!window.confirm(`删除自定义契约 ${contract.id}？`)) return
            try {
              await deleteReportContract(appName, contract.id)
              setContracts(await fetchReportContracts(appName))
              onToast(`已删除 ${appName} 的自定义契约 ${contract.id}`)
            } catch (error) { onToast(`删除失败: ${apiErrorMessage(error)}`, false) }
          }}>删除</button>}
        </div>)}

        <div style={sectionTitle}>模型 OTA</div>
        <Field label="平台 WebSocket 地址"><input style={inputStyle} value={ota.platform_ws_host}
          onChange={event => { markDirty(); setOta(current => ({ ...current, platform_ws_host: event.target.value })) }} /></Field>
      </>}
    </div>
    <div className="app-integration-footer">
      {dirty && <span className="app-integration-dirty">● 有未保存修改</span>}
      <button type="button" className="app-integration-button app-integration-button-primary"
        onClick={save} disabled={saving || loading}>{saving ? '保存中…' : `保存到 ${appName}`}</button>
    </div>
  </div></div>
}
