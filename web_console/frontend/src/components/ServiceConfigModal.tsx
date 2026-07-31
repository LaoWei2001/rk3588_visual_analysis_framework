import { useEffect, useState, type ReactNode } from 'react'
import {
  fetchDeliveryAdapters, fetchOtaConfig, fetchUploadConfig,
  saveOtaConfig, saveUploadConfig,
  type AdapterProfileField, type DeliveryAdapterDef, type OtaConfig,
  type UploadProfile, type UploadServiceConfig,
} from '../api/client'
import NumberField from './NumberField'
import { useEditorStore } from '../store/editorStore'

interface Props {
  appName: string
  onClose: () => void
  onToast: (msg: string, ok?: boolean) => void
  embedded?: boolean
}

const EMPTY_UPLOAD: UploadServiceConfig = { profiles: {} }
const EMPTY_OTA: OtaConfig = { platform_ws_host: 'tunnel.memanager.cn', target_config: 'active' }
const inputStyle: React.CSSProperties = {
  background: '#0f1117', color: '#e6e9ef', border: '1px solid #2e3352',
  borderRadius: 6, padding: '6px 8px', fontSize: 13, width: '100%',
}
const sectionTitle: React.CSSProperties = {
  fontSize: 13, fontWeight: 600, color: '#7aa2f7',
  margin: '18px 0 8px', borderBottom: '1px solid #2e3352', paddingBottom: 6,
}

function Field({ label, children }: { label: string; children: ReactNode }) {
  return <label style={{ display: 'flex', flexDirection: 'column', gap: 4, marginBottom: 10 }}>
    <span style={{ fontSize: 12, color: '#9aa4b2' }}>{label}</span>
    {children}
  </label>
}

function JsonProfileField({
  definition, value, onChange, onError, onEdit,
}: {
  definition: AdapterProfileField
  value: unknown
  onChange: (value: unknown) => void
  onError: (message: string) => void
  onEdit: () => void
}) {
  const [text, setText] = useState(() => JSON.stringify(value ?? definition.default ?? {}, null, 2))
  useEffect(() => {
    setText(JSON.stringify(value ?? definition.default ?? {}, null, 2))
  }, [value, definition.default])
  return <textarea style={{ ...inputStyle, minHeight: 86, fontFamily: 'monospace' }}
    value={text} onChange={event => {
      setText(event.target.value)
      onEdit()
    }}
    onBlur={() => {
      try { onChange(JSON.parse(text)) }
      catch { onError(`${definition.label} 必须是有效 JSON`) }
    }} />
}

function ProfileField({
  definition, value, onChange, onError, onEdit,
}: {
  definition: AdapterProfileField
  value: unknown
  onChange: (value: unknown) => void
  onError: (message: string) => void
  onEdit: () => void
}) {
  const actual = value ?? definition.default ?? ''
  if (definition.type === 'number') {
    return <NumberField style={inputStyle} def={Number(definition.default ?? 0)}
      value={typeof actual === 'number' ? actual : Number(actual)}
      onChange={next => onChange(next ?? definition.default ?? 0)} />
  }
  if (definition.type === 'select') {
    return <select style={inputStyle} value={String(actual)}
      onChange={event => onChange(event.target.value)}>
      {(definition.options ?? []).map(option => <option key={option} value={option}>{option}</option>)}
    </select>
  }
  if (definition.type === 'json') {
    return <JsonProfileField definition={definition} value={actual}
      onChange={onChange} onError={onError} onEdit={onEdit} />
  }
  return <input style={inputStyle} type={definition.type === 'secret' ? 'password' : 'text'}
    value={String(actual)} onChange={event => onChange(event.target.value)}
    placeholder={definition.required ? '必填' : ''} />
}

export default function ServiceConfigModal({ appName, onClose, onToast, embedded = false }: Props) {
  const [upload, setUpload] = useState<UploadServiceConfig>(EMPTY_UPLOAD)
  const [adapters, setAdapters] = useState<DeliveryAdapterDef[]>([])
  const [ota, setOta] = useState<OtaConfig>(EMPTY_OTA)
  const [loading, setLoading] = useState(true)
  const [saving, setSaving] = useState(false)
  const setUploadProfiles = useEditorStore(state => state.setUploadProfiles)
  const serviceConfigDirty = useEditorStore(state => state.serviceConfigDirty)
  const setServiceConfigDirty = useEditorStore(state => state.setServiceConfigDirty)

  const markDirty = () => setServiceConfigDirty(true)

  useEffect(() => {
    let alive = true
    Promise.all([fetchUploadConfig(appName), fetchDeliveryAdapters(appName), fetchOtaConfig(appName)])
      .then(([config, catalog, otaConfig]) => {
        if (!alive) return
        setUpload({ profiles: config.profiles ?? {} })
        setAdapters(catalog)
        setOta({ ...EMPTY_OTA, ...otaConfig })
        setServiceConfigDirty(false)
      })
      .catch(error => onToast(`加载服务配置失败: ${String(error)}`, false))
      .finally(() => { if (alive) setLoading(false) })
    return () => {
      alive = false
      setServiceConfigDirty(false)
    }
  }, [appName, onToast, setServiceConfigDirty])

  useEffect(() => {
    if (!serviceConfigDirty) return
    const onBeforeUnload = (event: BeforeUnloadEvent) => event.preventDefault()
    window.addEventListener('beforeunload', onBeforeUnload)
    return () => window.removeEventListener('beforeunload', onBeforeUnload)
  }, [serviceConfigDirty])

  const patchProfile = (id: string, patch: Partial<UploadProfile>) => {
    markDirty()
    setUpload(current => ({
      profiles: {
        ...current.profiles,
        [id]: { ...current.profiles[id], ...patch } as UploadProfile,
      },
    }))
  }
  const addProfile = () => {
    if (adapters.length === 0) return
    markDirty()
    let index = 1
    while (upload.profiles[`connection_${index}`]) index += 1
    const id = `connection_${index}`
    const adapter = adapters[0]
    const defaults = Object.fromEntries(adapter.profile_fields
      .filter(field => field.default !== undefined)
      .map(field => [field.key, field.default]))
    setUpload(current => ({
      profiles: { ...current.profiles, [id]: { adapter: adapter.id, ...defaults } },
    }))
  }
  const removeProfile = (id: string) => {
    markDirty()
    setUpload(current => {
      const profiles = { ...current.profiles }
      delete profiles[id]
      return { profiles }
    })
  }
  const renameProfile = (oldId: string, rawId: string) => {
    const id = rawId.trim()
    if (!id || id === oldId) return
    if (!/^[A-Za-z0-9_-]+$/.test(id) || upload.profiles[id]) {
      onToast('Profile ID 必须唯一，且只能包含字母、数字、下划线和短横线', false)
      return
    }
    markDirty()
    setUpload(current => {
      const profiles = { ...current.profiles, [id]: current.profiles[oldId] }
      delete profiles[oldId]
      return { profiles }
    })
  }
  const changeAdapter = (id: string, adapterId: string) => {
    const adapter = adapters.find(item => item.id === adapterId)
    if (!adapter) return
    markDirty()
    const defaults = Object.fromEntries(adapter.profile_fields
      .filter(field => field.default !== undefined)
      .map(field => [field.key, field.default]))
    setUpload(current => ({
      profiles: { ...current.profiles, [id]: { adapter: adapterId, ...defaults } },
    }))
  }
  const save = async () => {
    setSaving(true)
    try {
      await Promise.all([saveUploadConfig(appName, upload), saveOtaConfig(appName, ota)])
      setUploadProfiles(upload.profiles)
      setServiceConfigDirty(false)
      onToast('服务配置已保存 ✓')
      if (!embedded) onClose()
    } catch (error) {
      onToast(`保存失败: ${error instanceof Error ? error.message : String(error)}`, false)
    } finally {
      setSaving(false)
    }
  }
  const close = () => {
    if (serviceConfigDirty &&
        !window.confirm('服务参数有未保存的改动，确定关闭？未保存的修改将丢失。')) return
    setServiceConfigDirty(false)
    onClose()
  }

  const content = <div style={{
    background: '#1a1d29', color: '#e6e9ef', borderRadius: 10,
    width: embedded ? 'min(720px, 100%)' : 620, maxWidth: '92vw',
    margin: embedded ? '0 auto' : undefined,
    border: '1px solid #2e3352', overflow: 'hidden',
  }}>
    <div style={{ padding: '14px 18px', borderBottom: '1px solid #2e3352', fontWeight: 600 }}>
      ⚙ 服务参数 — {appName}
    </div>
    <div style={{ padding: '6px 18px 18px', maxHeight: embedded ? undefined : '75vh', overflowY: 'auto' }}>
      {loading ? <div style={{ padding: 40, textAlign: 'center' }}>加载中…</div> : <>
        <div style={sectionTitle}>📡 投递连接 Profile</div>
        <div style={{ fontSize: 11, color: '#9aa4b2', lineHeight: 1.6, marginBottom: 12 }}>
          每个连接明确选择一个适配器。新增适配器后，字段表单会根据适配器目录自动生成。
        </div>
        {Object.entries(upload.profiles).map(([id, profile]) => {
          const adapter = adapters.find(item => item.id === profile.adapter)
          return <div key={id} style={{
            border: '1px solid #2e3352', borderRadius: 8, padding: 12, marginBottom: 18,
          }}>
            <div style={{ display: 'grid', gridTemplateColumns: '1fr 170px auto', gap: 8, marginBottom: 10 }}>
              <input style={inputStyle} defaultValue={id} key={id}
                onBlur={event => renameProfile(id, event.target.value)} />
              <select style={inputStyle} value={profile.adapter}
                onChange={event => changeAdapter(id, event.target.value)}>
                {adapters.map(item => <option key={item.id} value={item.id}>{item.label}</option>)}
              </select>
              <button type="button" onClick={() => removeProfile(id)}
                style={{ background: '#7f1d1d', color: '#fff', border: 0, borderRadius: 6, padding: '6px 10px' }}>
                删除
              </button>
            </div>
            {adapter?.profile_fields.map(definition => <Field key={definition.key}
              label={`${definition.label}${definition.required ? ' *' : ''}`}>
              <ProfileField definition={definition} value={profile[definition.key]}
                onChange={value => patchProfile(id, { [definition.key]: value })}
                onError={message => onToast(message, false)} onEdit={markDirty} />
            </Field>)}
          </div>
        })}
        <button type="button" onClick={addProfile} disabled={adapters.length === 0}
          style={{ width: '100%', background: '#2563eb', color: '#fff', border: 0, borderRadius: 6, padding: 9 }}>
          ＋新增连接
        </button>

        <div style={sectionTitle}>⬇ 模型 OTA 升级服务</div>
        <Field label="平台 WebSocket 地址">
          <input style={inputStyle} value={ota.platform_ws_host}
            onChange={event => {
              markDirty()
              setOta(current => ({ ...current, platform_ws_host: event.target.value }))
            }} />
        </Field>
        <Field label="目标配置文件名">
          <input style={inputStyle} value={ota.target_config}
            onChange={event => {
              markDirty()
              setOta(current => ({ ...current, target_config: event.target.value }))
            }} />
        </Field>
      </>}
    </div>
    <div style={{ display: 'flex', justifyContent: 'flex-end', gap: 10, padding: 12, borderTop: '1px solid #2e3352' }}>
      {serviceConfigDirty && <span style={{ marginRight: 'auto', color: '#f59e0b', fontSize: 12 }}>
        ● 有未保存修改
      </span>}
      {!embedded && <button onClick={close}>取消</button>}
      <button onClick={save} disabled={saving || loading}
        style={{ background: '#3b82f6', color: '#fff', border: 0, borderRadius: 6, padding: '8px 16px' }}>
        {saving ? '保存中…' : '保存'}
      </button>
    </div>
  </div>

  if (embedded) return content
  return <div onClick={close} style={{
    position: 'fixed', inset: 0, background: 'rgba(0,0,0,.55)', zIndex: 1000,
    display: 'flex', alignItems: 'center', justifyContent: 'center',
  }}><div onClick={event => event.stopPropagation()}>{content}</div></div>
}
