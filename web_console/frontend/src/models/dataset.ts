/** Dataset domain values shared by transport, state and presentation code.
 * Keep this module independent of HTTP, stores and React components.
 */
export interface DatasetConfig {
  rules: unknown[]; interval_sec: number; confirm_sec: number; max_samples: number;
  images_per_folder: number; duration_hours: number;
  roi_name: string; roi_anchor: string; trigger_mode: string;
}

export interface DatasetDraft {
  taskId: string | null
  channelId: number
  config: DatasetConfig
  original: string
}

export interface DatasetTask {
  id: string; channel_id: number; config: DatasetConfig; enabled: boolean; saved: number;
  deadline: number; pending: number; matched: number; skipped: number; status: string;
  manual_id?: string; manual_state?: string; manual_error?: string; manual_wait_ms?: number; manual_frame?: number;
  current_files: number | null; files_updated_at: number; files_error: string;
}

export interface DatasetStatus {
  manual_capture_supported?: boolean;
  tasks: DatasetTask[];
  channels: Array<{id: number; logic: string; labels: string[]; threshold: number; eligible: boolean; rois: string[]}>;
  receiver: {online: boolean; directory: string; error: string; last_seen: number; hotkeys_available?: boolean;
    control_task?: {id: string; app: string; task_id: string; channel_id: number} | null}; engine_error: string;
}

export type DatasetReceiverPlatform = 'windows-amd64' | 'python'

export const removeLegacyImageQuality = (config: DatasetConfig): DatasetConfig => {
  const current = {...config} as DatasetConfig & {jpeg_quality?: unknown}
  delete current.jpeg_quality
  return current
}
