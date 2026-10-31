import { api, sharedRead } from './http'

// ── 当前程序包的投递连接、契约模板与 OTA 配置 ──
export interface DeliveryConnection {
  adapter: string
  [key: string]: unknown
}
export interface DeliveryConnectionsConfig {
  connections: Record<string, DeliveryConnection>
}
export interface AdapterConnectionField {
  key: string
  label: string
  type: 'string' | 'secret' | 'number' | 'json' | 'select'
  required?: boolean
  default?: unknown
  options?: string[]
}
export interface DeliveryAdapterDef {
  id: string
  label: string
  supported_media: Array<'annotated_image' | 'raw_image' | 'video'>
  connection_fields: AdapterConnectionField[]
  transforms: string[]
}
export interface ReportContract {
  id: string                                                // 框架内部键，不在界面或导出模板中呈现
  version: number
  label: string
  description?: string
  owner_logic?: string
  event_types: string[]
  adapter: string
  media: Array<'annotated_image' | 'raw_image' | 'video'>
  mapping: Array<{
    source: string
    target: string
    value?: unknown
    type?: string
    transform?: string
    location?: 'body' | 'query' | 'form' | 'header' | 'file'
    file_mode?: 'single' | 'list'
    required?: boolean
  }>
  request?: Record<string, unknown>
  success?: Record<string, unknown>
  origin?: 'package' | 'custom'
  package_template?: boolean
  revision: string
}
export interface OtaConfig {
  platform_ws_host: string
}

export const fetchConnections = (name: string) =>
  api.get<DeliveryConnectionsConfig>(`/apps/${name}/connections`).then(r => r.data)
export const saveConnections = (name: string, cfg: DeliveryConnectionsConfig) =>
  api.put(`/apps/${name}/connections`, cfg).then(r => r.data)
export const fetchDeliveryAdapters = (name: string) =>
  api.get<{ adapters: DeliveryAdapterDef[] }>(`/apps/${name}/delivery-adapters`).then(r => r.data.adapters)
export const fetchReportContracts = (name: string) =>
  sharedRead(`contracts:${name}`, () => api.get<{ contracts: ReportContract[] }>(`/apps/${name}/report-contracts`, {
    params: { refresh: Date.now() },
  }).then(r => r.data.contracts))
export const saveReportContract = (name: string, contract: ReportContract) =>
  api.put<{ ok: boolean; contract: ReportContract }>(
    `/apps/${name}/report-contracts/${encodeURIComponent(contract.id)}`,
    contract,
  ).then(r => r.data.contract)
export const deleteReportContract = (name: string, contractId: string) =>
  api.delete(`/apps/${name}/report-contracts/${encodeURIComponent(contractId)}`).then(r => r.data)
export const previewDelivery = (
  name: string,
  delivery: Record<string, unknown>,
  eventId = '',
  send = false,
) => api.post(`/apps/${name}/delivery-preview`, {
  delivery, event_id: eventId, send,
}).then(r => r.data as { preview: Record<string, unknown>; test?: Record<string, unknown> })
export const fetchOtaConfig = (name: string) =>
  api.get<OtaConfig>(`/apps/${name}/ota-config`).then(r => r.data)
export const saveOtaConfig = (name: string, cfg: OtaConfig) =>
  api.post(`/apps/${name}/ota-config`, cfg).then(r => r.data)
