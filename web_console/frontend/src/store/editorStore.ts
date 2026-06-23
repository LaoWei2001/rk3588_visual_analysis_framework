import { create } from 'zustand'
import type { AppAssets } from '../api/client'
import { fetchAssets } from '../api/client'

interface EditorState {
  appName: string
  assets: AppAssets
  globalMaxFps: number   // 全局最大FPS：影响 USB 采集分辨率/视野；ROI 抓帧需与之一致(镜像 C++ desired_fps 兜底)
  dirty: boolean         // 编辑器画布有未保存改动；供侧边栏导航拦截使用（离开前提示）
  setAppName: (name: string) => void
  setGlobalMaxFps: (fps: number) => void
  setDirty: (d: boolean) => void
  loadAssets: (name: string) => Promise<void>
}

const EMPTY_ASSETS: AppAssets = { models: [], labels: [], videos: [] }

export const useEditorStore = create<EditorState>((set) => ({
  appName: '',
  assets:  EMPTY_ASSETS,
  globalMaxFps: 15,
  dirty: false,
  setAppName: (name) => set({ appName: name }),
  setGlobalMaxFps: (fps) => set({ globalMaxFps: fps > 0 ? fps : 15 }),
  setDirty: (d) => set({ dirty: d }),
  loadAssets: async (name) => {
    try {
      const assets = await fetchAssets(name)
      set({ assets })
    } catch {
      set({ assets: EMPTY_ASSETS })
    }
  },
}))
