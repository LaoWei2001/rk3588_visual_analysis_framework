import axios from 'axios'
import { api } from './http'
import type {DatasetConfig, DatasetStatus, DatasetTask, DatasetReceiverPlatform} from '../models/dataset'

// Preserve established API imports; domain consumers should import models/dataset.
export type {DatasetConfig, DatasetStatus, DatasetTask, DatasetReceiverPlatform} from '../models/dataset'
export {removeLegacyImageQuality} from '../models/dataset'
export const fetchDataset = (name: string) => api.get<DatasetStatus>(`/apps/${encodeURIComponent(name)}/dataset`).then(r => r.data)
export const saveDataset = (name: string, channel: number, config: DatasetConfig) =>
  api.put<DatasetStatus>(`/apps/${encodeURIComponent(name)}/dataset/channels/${channel}`, config).then(r => r.data)
export const enableDataset = (name: string, id: string, enabled: boolean) =>
  api.post<DatasetStatus>(`/apps/${encodeURIComponent(name)}/dataset/tasks/${id}/enabled`, {enabled}).then(r => r.data)
export const deleteDataset = (name: string, id: string) =>
  api.delete<DatasetStatus>(`/apps/${encodeURIComponent(name)}/dataset/tasks/${id}`).then(r => r.data)
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


export const captureDataset = (name: string, id: string, request_id: string) =>
  api.post<Pick<DatasetTask, "manual_id" | "manual_state" | "manual_error" | "manual_wait_ms" | "manual_frame">>(`/apps/${encodeURIComponent(name)}/dataset/tasks/${id}/capture`, {request_id}).then(r => r.data)
