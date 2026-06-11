import { create } from 'zustand'
import type { AppAssets } from '../api/client'
import { fetchAssets } from '../api/client'

interface EditorState {
  appName: string
  assets: AppAssets
  globalMaxFps: number   // 全局最大FPS：影响 USB 采集分辨率/视野；ROI 抓帧需与之一致(镜像 C++ desired_fps 兜底)
  setAppName: (name: string) => void
  setGlobalMaxFps: (fps: number) => void
  loadAssets: (name: string) => Promise<void>
}

const EMPTY_ASSETS: AppAssets = { models: [], labels: [], videos: [] }

export const useEditorStore = create<EditorState>((set) => ({
  appName: '',
  assets:  EMPTY_ASSETS,
  globalMaxFps: 15,
  setAppName: (name) => set({ appName: name }),
  setGlobalMaxFps: (fps) => set({ globalMaxFps: fps > 0 ? fps : 15 }),
  loadAssets: async (name) => {
    try {
      const assets = await fetchAssets(name)
      set({ assets })
    } catch {
      set({ assets: EMPTY_ASSETS })
    }
  },
}))
