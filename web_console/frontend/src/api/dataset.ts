import axios from 'axios'
import { api } from './http'

export interface DatasetConfig {
  rules: unknown[]; interval_sec: number; confirm_sec: number; max_samples: number;
  images_per_folder: number; duration_hours: number; jpeg_quality: number;
  roi_name: string; roi_anchor: string; trigger_mode: string;
}
export interface DatasetTask {
  id: string; channel_id: number; config: DatasetConfig; enabled: boolean; saved: number;
  deadline: number; pending: number; matched: number; skipped: number; status: string;
  current_files: number | null; files_updated_at: number; files_error: string;
}
export interface DatasetStatus {
  tasks: DatasetTask[];
  channels: Array<{id: number; logic: string; labels: string[]; threshold: number; eligible: boolean; rois: string[]}>;
  receiver: {online: boolean; directory: string; error: string; last_seen: number}; engine_error: string;
}
export const fetchDataset = (name: string) => api.get<DatasetStatus>(`/apps/${encodeURIComponent(name)}/dataset`).then(r => r.data)
export const saveDataset = (name: string, channel: number, config: DatasetConfig) =>
  api.put<DatasetStatus>(`/apps/${encodeURIComponent(name)}/dataset/channels/${channel}`, config).then(r => r.data)
export const enableDataset = (name: string, id: string, enabled: boolean) =>
  api.post<DatasetStatus>(`/apps/${encodeURIComponent(name)}/dataset/tasks/${id}/enabled`, {enabled}).then(r => r.data)
export const deleteDataset = (name: string, id: string) =>
  api.delete<DatasetStatus>(`/apps/${encodeURIComponent(name)}/dataset/tasks/${id}`).then(r => r.data)
export type DatasetReceiverPlatform = 'windows-amd64' | 'python'
export const downloadDatasetReceiver = async (url: string, platform: DatasetReceiverPlatform = 'python') => {
  try {
    return (await api.post<Blob>('/dataset/receiver-package', {url, platform}, {responseType: 'blob'})).data
  } catch (error) {
    // Error responses are JSON even when successful downloads are binary.
    if (axios.isAxiosError(error) && error.response?.data instanceof Blob) {
      try { error.response.data = JSON.parse(await error.response.data.text()) } catch { /* keep original error */ }
    }
    throw error
  }
}

