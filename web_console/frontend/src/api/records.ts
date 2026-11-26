import { api } from './http'
import { useAuthStore } from '../store/authStore'

// ── 本地事件发件箱 ──────────────────────────────────────────────────────────
export interface EventRecord {
  id: string
  channel_id: number | null
  event_type: string
  required_media: string[]
  trigger_count?: number
  snap_time: string | number
  created_unix_sec: number
  has_raw_image: boolean
  has_annotated_image: boolean
  has_video: boolean
  message?: string
  state: string
  media_statuses?: Record<string, { status: string; error?: string }>
  total_bytes?: number
  deliveries: Array<{
    id?: string; media?: string[]; connection_id?: string; contract_id?: string; contract_label?: string
    contract_revision?: string; status?: string
    attempts?: number; last_error?: string
  }>
}
export interface RecordsResp {
  matching_ids: string[]
  raw_available: Record<string, boolean>
  records: EventRecord[]
  event_types: string[]
  count: number
  filtered_count: number
  total_bytes: number
  cap_bytes: number
}

export const fetchRecords = (
  name: string,
  limit = 500,
  filters?: { startUnixMs?: number; endUnixMs?: number; eventType?: string; mediaType?: string },
) => api.get<RecordsResp>(`/apps/${name}/records`, {
  params: {
    limit,
    media_type: filters?.mediaType,
    start_unix_ms: filters?.startUnixMs,
    end_unix_ms: filters?.endUnixMs,
    event_type: filters?.eventType || undefined,
  },
}).then(r => r.data)

export interface RecordJsonResponse {
  schema_version: number
  event: Record<string, unknown>
  source: Record<string, unknown>
  fields: Record<string, unknown>
  media: Record<string, string>
  media_statuses: Record<string, { status: string; error?: string }>
  deliveries: Array<Record<string, unknown>>
}

export const fetchRecordJson = (name: string, id: string) =>
  api.get<RecordJsonResponse>(
    `/apps/${encodeURIComponent(name)}/records/${encodeURIComponent(id)}/json`,
  ).then(r => r.data)

// <img> 无法带 Authorization 头，token 走查询参数（后端 auth_middleware 已放行）
export const recordImageUrl = (name: string, id: string, raw = false): string => {
  const token = useAuthStore.getState().token ?? ''
  return `/api/apps/${encodeURIComponent(name)}/records/${encodeURIComponent(id)}/image`
    + `?raw=${raw ? 1 : 0}&token=${encodeURIComponent(token)}`
}

export const recordVideoUrl = (name: string, id: string): string => {
  const token = useAuthStore.getState().token ?? ''
  return `/api/apps/${encodeURIComponent(name)}/records/${encodeURIComponent(id)}/video`
    + `?token=${encodeURIComponent(token)}`
}

export const retryRecord = (name: string, id: string) =>
  api.post(`/apps/${name}/records/${id}/retry`).then(r => r.data)

export const deleteRecord = (name: string, id: string) =>
  api.delete(`/apps/${name}/records/${id}`).then(r => r.data)

export const deleteAllRecords = (name: string) =>
  api.delete(`/apps/${name}/records`).then(r => r.data)

export const deleteSelectedRecords = (name: string, ids: string[]) =>
  api.post<{ ok: boolean; deleted_ids: string[]; failed_ids: string[] }>(
    `/apps/${encodeURIComponent(name)}/records/delete-selected`, { ids },
  ).then(r => r.data)

export const prepareRawImageExport = (name: string, ids?: string[]) =>
  api.post<{ download_id: string; filename: string; image_count: number; skipped_count: number;
    images: Array<{ id: string; filename: string }> }>(
    `/apps/${encodeURIComponent(name)}/records/export-images`, { ids: ids ?? null },
  ).then(r => r.data)

export const rawImageDownloadUrl = (name: string, id: string) =>
  `/api/apps/${encodeURIComponent(name)}/records/${encodeURIComponent(id)}/raw-image`
  + `?token=${encodeURIComponent(useAuthStore.getState().token ?? '')}`

export const rawImageExportUrl = (name: string, downloadId: string) =>
  `/api/apps/${encodeURIComponent(name)}/records/export-images/${encodeURIComponent(downloadId)}`
  + `?token=${encodeURIComponent(useAuthStore.getState().token ?? '')}`
