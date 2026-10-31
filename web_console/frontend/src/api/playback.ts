import { api } from './http'

// ── 本地文件视频播放进度（独立于业务 Logic）────────────────────────────
export interface FilePlaybackSource {
  owner_channel_id: number
  channel_ids: number[]
  location: string
  available: boolean
  seekable: boolean
  playing: boolean
  ended: boolean
  position_ms: number
  duration_ms: number
}

export interface FilePlaybackResponse {
  socket_ready: boolean
  sources: FilePlaybackSource[]
}

export const fetchFilePlayback = (name: string) =>
  api.get<FilePlaybackResponse>(`/apps/${name}/file-playback`, {
    params: { _ts: Date.now() },
  }).then(r => r.data)

export const seekFilePlayback = (name: string, channelId: number, positionMs: number) =>
  api.post<{ ok: boolean; channel_id: number; position_ms: number; message?: string }>(
    `/apps/${name}/file-playback/${channelId}/seek`,
    { position_ms: Math.max(0, Math.round(positionMs)) },
  ).then(r => r.data)

// ── Live H264 fMP4 stream URL (由 MSE fetch，Authorization 走请求头) ──────────
export const streamUrl = (name: string): string =>
  `/api/apps/${encodeURIComponent(name)}/stream`
