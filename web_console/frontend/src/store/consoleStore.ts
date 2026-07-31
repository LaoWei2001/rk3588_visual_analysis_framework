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
          binary_name: 'vision_analysis',
          known_model_types: ['yolov5', 'yolov8_det', 'yolov8_pose', 'yolov5_seg'],
        },
      })
    }
  },
}))
