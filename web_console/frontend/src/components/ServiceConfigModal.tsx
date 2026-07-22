/**
 * ServiceConfigModal — 管理两个微服务的「服务级」参数，可作为弹窗或独立页面内嵌面板：
 *   - 上报服务 (config.yaml): 默认地址 / Profile / 超时
 *   - OTA 升级服务 (ota_config.json): 平台地址 / 目标配置文件名
 *
 * 每通道的上报节点只保存 profile_id；地址和密钥集中在这里管理。
 */
import { useEffect, useState, type ReactNode } from 'react'
import {
  fetchUploadConfig, saveUploadConfig, fetchOtaConfig, saveOtaConfig,
  type UploadServiceConfig, type UploadProfile, type OtaConfig,
} from '../api/client'
import NumberField from './NumberField'
import { useEditorStore } from '../store/editorStore'

interface Props {
  appName: string
  onClose: () => void
  onToast: (msg: string, ok?: boolean) => void
  embedded?: boolean
}

const EMPTY_UPLOAD: UploadServiceConfig = {
  dify:   { api_url: '', api_key: '', timeout: 120 },
  server: { url: '', timeout: 15 },
  profiles: {},
}
const EMPTY_OTA: OtaConfig = { platform_ws_host: 'tunnel.memanager.cn', target_config: 'config.json' }

// 服务器上报不使用鉴权 Token；读取旧配置时一并剔除，下一次保存会清理历史残留字段。
const stripServerTokens = (profiles: Record<string, UploadProfile>): Record<string, UploadProfile> =>
  Object.fromEntries(Object.entries(profiles).map(([id, profile]) => {
    const clean = { ...profile } as UploadProfile & { token?: string }
    delete clean.token
    return [id, clean]
  }))

const overlay: React.CSSProperties = {
  position: 'fixed', inset: 0, background: 'rgba(0,0,0,0.55)',
  display: 'flex', alignItems: 'center', justifyContent: 'center', zIndex: 1000,
}
const dialog: React.CSSProperties = {
  background: '#1a1d29', color: '#e6e9ef', borderRadius: 10, width: 560,
  maxWidth: '92vw', maxHeight: '88vh', overflowY: 'auto',
  border: '1px solid #2e3352', boxShadow: '0 12px 40px rgba(0,0,0,0.5)',
}
const inputStyle: React.CSSProperties = {
  background: '#0f1117', color: '#e6e9ef', border: '1px solid #2e3352',
  borderRadius: 6, padding: '6px 8px', fontSize: 13, width: '100%',
}
const sectionTitle: React.CSSProperties = {
  fontSize: 13, fontWeight: 600, color: '#7aa2f7',
  margin: '18px 0 8px', borderBottom: '1px solid #2e3352', paddingBottom: 6,
}

function Field({ label, children }: { label: string; children: ReactNode }) {
  return (
    <label style={{ display: 'flex', flexDirection: 'column', gap: 4, marginBottom: 10 }}>
      <span style={{ fontSize: 12, color: '#9aa4b2' }}>{label}</span>
      {children}
    </label>
  )
}

export default function ServiceConfigModal({ appName, onClose, onToast, embedded = false }: Props) {
  const [upload, setUpload] = useState<UploadServiceConfig>(EMPTY_UPLOAD)
  const [ota, setOta]       = useState<OtaConfig>(EMPTY_OTA)
  const [loading, setLoading] = useState(true)
  const [saving, setSaving]   = useState(false)
  const setUploadProfiles = useEditorStore(s => s.setUploadProfiles)

  useEffect(() => {
    let alive = true
    Promise.all([fetchUploadConfig(appName), fetchOtaConfig(appName)])
      .then(([u, o]) => {
        if (!alive) return
        setUpload({
          dify:   { ...EMPTY_UPLOAD.dify,   ...u.dify },
          server: { ...EMPTY_UPLOAD.server, ...u.server },
          profiles: stripServerTokens(u.profiles ?? {}),
        })
        setOta({ ...EMPTY_OTA, ...o })
      })
      .catch(() => onToast('加载服务配置失败', false))
      .finally(() => { if (alive) setLoading(false) })
    return () => { alive = false }
  }, [appName, onToast])

  const patchDify   = (p: Partial<UploadServiceConfig['dify']>)   => setUpload(s => ({ ...s, dify:   { ...s.dify,   ...p } }))
  const patchServer = (p: Partial<UploadServiceConfig['server']>) => setUpload(s => ({ ...s, server: { ...s.server, ...p } }))
  const patchProfile = (id: string, patch: Partial<UploadProfile>) =>
    setUpload(s => ({ ...s, profiles: { ...s.profiles, [id]: { ...s.profiles[id], ...patch } } }))
  const nextProfileId = (type: 'server' | 'dify') => {
    let index = 1
    while (upload.profiles[`${type}_${index}`]) index += 1
    return `${type}_${index}`
  }
  const addProfile = (type: 'server' | 'dify') => {
    const id = nextProfileId(type)
    setUpload(s => ({ ...s, profiles: { ...s.profiles, [id]: { type } } }))
  }
  const removeProfile = (id: string) => setUpload(s => {
    const profiles = { ...s.profiles }
    delete profiles[id]
    return { ...s, profiles }
  })
  const renameProfile = (oldId: string, rawId: string) => {
    const id = rawId.trim()
    if (!id || id === oldId) return
    if (!/^[A-Za-z0-9_-]+$/.test(id)) {
      onToast('Profile ID 只能使用字母、数字、下划线和短横线', false)
      return
    }
    if (upload.profiles[id]) {
      onToast(`Profile ID ${id} 已存在`, false)
      return
    }
    setUpload(s => {
      const profiles = { ...s.profiles, [id]: s.profiles[oldId] }
      delete profiles[oldId]
      return { ...s, profiles }
    })
  }

  const save = async () => {
    setSaving(true)
    try {
      await Promise.all([saveUploadConfig(appName, upload), saveOtaConfig(appName, ota)])
      setUploadProfiles(upload.profiles ?? {})
      onToast('服务配置已保存 ✓')
      if (!embedded) onClose()
    } catch (e: unknown) {
      onToast(`保存失败: ${e instanceof Error ? e.message : String(e)}`, false)
    } finally { setSaving(false) }
  }

  return (
    <div style={embedded ? undefined : overlay} onClick={embedded ? undefined : onClose}>
      <div style={embedded ? {
        ...dialog, width: '100%', maxWidth: 760, maxHeight: 'none', overflowY: 'visible',
        boxShadow: 'none', margin: '0 auto',
      } : dialog} onClick={e => e.stopPropagation()}>
        <div style={{
          display: 'flex', alignItems: 'center', justifyContent: 'space-between',
          padding: '14px 18px', borderBottom: '1px solid #2e3352', position: embedded ? 'static' : 'sticky',
          top: 0, background: '#1a1d29',
        }}>
          <span style={{ fontWeight: 600 }}>⚙ 服务参数 — {appName}</span>
          {!embedded && <button onClick={onClose} style={{ background: 'none', border: 'none', color: '#9aa4b2', fontSize: 18, cursor: 'pointer' }}>✕</button>}
        </div>

        <div style={{ padding: '6px 18px 18px' }}>
          {loading ? (
            <div style={{ padding: 40, textAlign: 'center', color: '#9aa4b2' }}>加载中…</div>
          ) : (
            <>
              {/* ── 上报服务 ── */}
              <div style={sectionTitle}>📡 上报服务 · 默认地址（通道留空时用）</div>

              <Field label="HTTP 上报默认地址 (server.url)">
                <input style={inputStyle} value={upload.server.url}
                  onChange={e => patchServer({ url: e.target.value })}
                  placeholder="http://192.168.2.22:9200/api/objectInvadeDet" />
              </Field>
              <Field label="HTTP 超时 (秒)">
                <NumberField style={inputStyle} def={15} value={upload.server.timeout}
                  onChange={v => patchServer({ timeout: v ?? 15 })} />
              </Field>

              <Field label="Dify 默认地址 (dify.api_url)">
                <input style={inputStyle} value={upload.dify.api_url}
                  onChange={e => patchDify({ api_url: e.target.value })}
                  placeholder="http://192.168.2.98:8015" />
              </Field>
              <Field label="Dify 默认 API Key">
                <input style={inputStyle} type="text" value={upload.dify.api_key}
                  onChange={e => patchDify({ api_key: e.target.value })}
                  placeholder="app-xxxxxxxx" />
              </Field>
              <Field label="Dify 超时 (秒)">
                <NumberField style={inputStyle} def={120} value={upload.dify.timeout}
                  onChange={v => patchDify({ timeout: v ?? 120 })} />
              </Field>

              <div style={sectionTitle}>🔐 可复用上报连接 Profile</div>
              <div style={{ fontSize: 11, color: '#9aa4b2', lineHeight: 1.6, marginBottom: 10 }}>
                每一路通道的投递任务可选择不同 Profile。留空使用上面的默认连接。
              </div>
              {Object.entries(upload.profiles).map(([id, profile]) => (
                <div key={id} style={{ border: '1px solid #2e3352', borderRadius: 8, padding: 12, marginBottom: 10 }}>
                  <div style={{ display: 'grid', gridTemplateColumns: '1fr 110px auto', gap: 8, alignItems: 'center', marginBottom: 10 }}>
                    <input style={inputStyle} defaultValue={id} key={id}
                      title="Profile ID（画布上报节点通过此 ID 引用）"
                      onBlur={e => renameProfile(id, e.target.value)} />
                    <select style={inputStyle} value={profile.type ?? 'server'}
                      onChange={e => patchProfile(id, { type: e.target.value as 'server' | 'dify' })}>
                      <option value="server">服务器</option>
                      <option value="dify">Dify</option>
                    </select>
                    <button type="button" onClick={() => removeProfile(id)}
                      style={{ background: '#7f1d1d', color: '#fff', border: 'none', borderRadius: 6, padding: '7px 10px', cursor: 'pointer' }}>
                      删除
                    </button>
                  </div>
                  {(profile.type ?? 'server') === 'server' ? <>
                    <Field label="服务器上报地址">
                      <input style={inputStyle} value={profile.url ?? ''}
                        onChange={e => patchProfile(id, { url: e.target.value })}
                        placeholder="http://server.example.com/api/alarm" />
                    </Field>
                    <Field label="HTTP 超时（秒）">
                      <NumberField style={inputStyle} def={upload.server.timeout} value={profile.timeout ?? upload.server.timeout}
                        onChange={v => patchProfile(id, { timeout: v ?? upload.server.timeout })} />
                    </Field>
                  </> : <>
                    <Field label="Dify API 地址">
                      <input style={inputStyle} value={profile.api_url ?? ''}
                        onChange={e => patchProfile(id, { api_url: e.target.value })}
                        placeholder="http://dify.example.com" />
                    </Field>
                    <Field label="Dify API Key">
                      <input style={inputStyle} type="text" value={profile.api_key ?? ''}
                        onChange={e => patchProfile(id, { api_key: e.target.value })} />
                    </Field>
                  </>}
                </div>
              ))}
              {Object.keys(upload.profiles).length === 0 && (
                <div style={{ padding: '14px 10px', textAlign: 'center', color: '#6b7280', fontSize: 12 }}>
                  还没有独立连接 Profile
                </div>
              )}
              <div style={{ display: 'flex', gap: 8, marginBottom: 12 }}>
                <button type="button" onClick={() => addProfile('server')}
                  style={{ flex: 1, background: '#2563eb', color: '#fff', border: 'none', borderRadius: 6, padding: '8px 10px', cursor: 'pointer' }}>
                  ＋服务器 Profile
                </button>
                <button type="button" onClick={() => addProfile('dify')}
                  style={{ flex: 1, background: '#7c3aed', color: '#fff', border: 'none', borderRadius: 6, padding: '8px 10px', cursor: 'pointer' }}>
                  ＋Dify Profile
                </button>
              </div>

              {/* ── OTA 升级服务 ── */}
              <div style={sectionTitle}>⬇ 模型 OTA 升级服务</div>
              <Field label="平台 WebSocket 地址 (platform_ws_host)">
                <input style={inputStyle} value={ota.platform_ws_host}
                  onChange={e => setOta(s => ({ ...s, platform_ws_host: e.target.value }))}
                  placeholder="tunnel.memanager.cn" />
              </Field>
              <Field label="目标配置文件名 (相对 assets/，默认 config.json)">
                <input style={inputStyle} value={ota.target_config}
                  onChange={e => setOta(s => ({ ...s, target_config: e.target.value }))}
                  placeholder="config.json" />
              </Field>
              <div style={{ fontSize: 11, color: '#9aa4b2', lineHeight: 1.5 }}>
                目标文件须与控制台/程序实际运行的那份一致（默认 <code>config.json</code>），否则 OTA 换的模型热重载不进正在跑的进程。
              </div>
            </>
          )}
        </div>

        <div style={{
          display: 'flex', justifyContent: 'flex-end', gap: 10, padding: '12px 18px',
          borderTop: '1px solid #2e3352', position: embedded ? 'static' : 'sticky', bottom: 0, background: '#1a1d29',
        }}>
          {!embedded && <button onClick={onClose} disabled={saving}
            style={{ background: '#2e3352', color: '#e6e9ef', border: 'none', borderRadius: 6, padding: '8px 16px', cursor: 'pointer' }}>取消</button>
          }
          <button onClick={save} disabled={saving || loading}
            style={{ background: '#3b82f6', color: '#fff', border: 'none', borderRadius: 6, padding: '8px 16px', cursor: 'pointer' }}>
            {saving ? '保存中…' : '💾 保存'}
          </button>
        </div>
      </div>
    </div>
  )
}
