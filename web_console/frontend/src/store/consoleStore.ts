import { create } from 'zustand'
import { ConsoleInfo, fetchConsoleInfo } from '../api/client'

interface ConsoleState {
  info: ConsoleInfo | null
  load: () => Promise<void>
}

export const useConsoleStore = create<ConsoleState>((set) => ({
  info: null,
  load: async () => {
    try {
      const info = await fetchConsoleInfo()
      set({ info })
    } catch {
      // backend not available yet — use defaults
      set({
        info: {
          version: '1.0.0',
          apps_root: '/opt/ai_apps',
          binary_name: 'rk3588_yolo',
          known_channel_logics: [
            'logic_default', 'logic_server', 'logic_dify', 'logic_hook',
            'logic_roll', 'logic_custom', 'logic_person_alarm',
            'logic_cross_camera', 'logic_dify_person_verify',
            'logic_roi', 'logic_multi_roi', 'logic_wafer', 'logic_wafer2',
            'logic_wafer_sop',
          ],
          known_global_logics: ['global_example', 'global_default'],
          known_model_types: ['yolov5', 'yolov8_det', 'yolov8_pose', 'yolov5_seg'],
        },
      })
    }
  },
}))
