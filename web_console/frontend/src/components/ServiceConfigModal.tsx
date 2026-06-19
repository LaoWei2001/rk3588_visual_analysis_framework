/**
 * ServiceConfigModal — 在一个弹窗里管理两个微服务的「服务级」参数：
 *   - 上报服务 (config.yaml): 默认地址 / Redis / 超时 (通道留空时的兜底值)
 *   - OTA 升级服务 (ota_config.json): 平台地址 / 目标配置文件名
 *
 * 注: 方案2 下每通道的上报地址在编辑器「上报配置」节点里逐通道填; 这里只管"默认/兜底"。
 */
import { useEffect, useState, type ReactNode } from 'react'
import {
  fetchUploadConfig, saveUploadConfig, fetchOtaConfig, saveOtaConfig,
  type UploadServiceConfig, type OtaConfig,
} from '../api/client'
import NumberField from './NumberField'

interface Props {
  appName: string
  onClose: () => void
  onToast: (msg: string, ok?: boolean) => void
}

const EMPTY_UPLOAD: UploadServiceConfig = {
  dify:   { api_url: '', api_key: '', timeout: 120 },
  server: { url: '', timeout: 15 },
  redis:  { host: 'localhost', port: 6379, db: 0, dify_queue: 'dify_queue', server_queue: 'server_queue' },
}
const EMPTY_OTA: OtaConfig = { platform_ws_host: 'tunnel.memanager.cn', target_config: 'config.json' }

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

export default function ServiceConfigModal({ appName, onClose, onToast }: Props) {
  const [upload, setUpload] = useState<UploadServiceConfig>(EMPTY_UPLOAD)
  const [ota, setOta]       = useState<OtaConfig>(EMPTY_OTA)
  const [loading, setLoading] = useState(true)
  const [saving, setSaving]   = useState(false)
  const [showRedis, setShowRedis] = useState(false)

  useEffect(() => {
    let alive = true
    Promise.all([fetchUploadConfig(appName), fetchOtaConfig(appName)])
      .then(([u, o]) => {
        if (!alive) return
        setUpload({
          dify:   { ...EMPTY_UPLOAD.dify,   ...u.dify },
          server: { ...EMPTY_UPLOAD.server, ...u.server },
          redis:  { ...EMPTY_UPLOAD.redis,  ...u.redis },
        })
        setOta({ ...EMPTY_OTA, ...o })
      })
      .catch(() => onToast('加载服务配置失败', false))
      .finally(() => { if (alive) setLoading(false) })
    return () => { alive = false }
  }, [appName, onToast])

  const patchDify   = (p: Partial<UploadServiceConfig['dify']>)   => setUpload(s => ({ ...s, dify:   { ...s.dify,   ...p } }))
  const patchServer = (p: Partial<UploadServiceConfig['server']>) => setUpload(s => ({ ...s, server: { ...s.server, ...p } }))
  const patchRedis  = (p: Partial<UploadServiceConfig['redis']>)  => setUpload(s => ({ ...s, redis:  { ...s.redis,  ...p } }))

  const save = async () => {
    setSaving(true)
    try {
      await Promise.all([saveUploadConfig(appName, upload), saveOtaConfig(appName, ota)])
      onToast('服务配置已保存 ✓')
      onClose()
    } catch (e: unknown) {
      onToast(`保存失败: ${e instanceof Error ? e.message : String(e)}`, false)
    } finally { setSaving(false) }
  }

  return (
    <div style={overlay} onClick={onClose}>
      <div style={dialog} onClick={e => e.stopPropagation()}>
        <div style={{
          display: 'flex', alignItems: 'center', justifyContent: 'space-between',
          padding: '14px 18px', borderBottom: '1px solid #2e3352', position: 'sticky',
          top: 0, background: '#1a1d29',
        }}>
          <span style={{ fontWeight: 600 }}>⚙ 服务配置 — {appName}</span>
          <button onClick={onClose} style={{ background: 'none', border: 'none', color: '#9aa4b2', fontSize: 18, cursor: 'pointer' }}>✕</button>
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
                <input style={inputStyle} type="password" value={upload.dify.api_key}
                  onChange={e => patchDify({ api_key: e.target.value })}
                  placeholder="app-xxxxxxxx" />
              </Field>
              <Field label="Dify 超时 (秒)">
                <NumberField style={inputStyle} def={120} value={upload.dify.timeout}
                  onChange={v => patchDify({ timeout: v ?? 120 })} />
              </Field>

              <button onClick={() => setShowRedis(v => !v)}
                style={{ background: 'none', border: 'none', color: '#7aa2f7', cursor: 'pointer', fontSize: 12, padding: '4px 0', marginBottom: 6 }}>
                {showRedis ? '▼' : '▶'} Redis 连接（高级，一般不用改）
              </button>
              {showRedis && (
                <div style={{ display: 'grid', gridTemplateColumns: '2fr 1fr 1fr', gap: 8 }}>
                  <Field label="host"><input style={inputStyle} value={upload.redis.host} onChange={e => patchRedis({ host: e.target.value })} /></Field>
                  <Field label="port"><NumberField style={inputStyle} def={6379} value={upload.redis.port} onChange={v => patchRedis({ port: v ?? 6379 })} /></Field>
                  <Field label="db"><NumberField style={inputStyle} def={0} value={upload.redis.db} onChange={v => patchRedis({ db: v ?? 0 })} /></Field>
                  <Field label="server_queue"><input style={inputStyle} value={upload.redis.server_queue} onChange={e => patchRedis({ server_queue: e.target.value })} /></Field>
                  <Field label="dify_queue"><input style={inputStyle} value={upload.redis.dify_queue} onChange={e => patchRedis({ dify_queue: e.target.value })} /></Field>
                </div>
              )}

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
          borderTop: '1px solid #2e3352', position: 'sticky', bottom: 0, background: '#1a1d29',
        }}>
          <button onClick={onClose} disabled={saving}
            style={{ background: '#2e3352', color: '#e6e9ef', border: 'none', borderRadius: 6, padding: '8px 16px', cursor: 'pointer' }}>取消</button>
          <button onClick={save} disabled={saving || loading}
            style={{ background: '#3b82f6', color: '#fff', border: 'none', borderRadius: 6, padding: '8px 16px', cursor: 'pointer' }}>
            {saving ? '保存中…' : '💾 保存'}
          </button>
        </div>
      </div>
    </div>
  )
}
