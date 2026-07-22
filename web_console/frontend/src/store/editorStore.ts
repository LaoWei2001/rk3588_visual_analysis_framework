import { create } from 'zustand'
import type { AppAssets, UploadProfile } from '../api/client'
import { fetchAssets, fetchUploadConfig } from '../api/client'

interface EditorState {
  appName: string
  assets: AppAssets
  uploadProfiles: Record<string, UploadProfile>
  globalMaxFps: number   // 全局最大FPS：影响 USB 采集分辨率/视野；ROI 抓帧需与之一致(镜像 C++ desired_fps 兜底)
  dirty: boolean         // 编辑器画布有未保存改动；供侧边栏导航拦截使用（离开前提示）
  setAppName: (name: string) => void
  setUploadProfiles: (profiles: Record<string, UploadProfile>) => void
  setGlobalMaxFps: (fps: number) => void
  setDirty: (d: boolean) => void
  loadAssets: (name: string) => Promise<void>
  loadUploadProfiles: (name: string) => Promise<void>
}

const EMPTY_ASSETS: AppAssets = { models: [], labels: [], videos: [] }

export const useEditorStore = create<EditorState>((set) => ({
  appName: '',
  assets:  EMPTY_ASSETS,
  uploadProfiles: {},
  globalMaxFps: 25,
  dirty: false,
  setAppName: (name) => set({ appName: name }),
  setUploadProfiles: (uploadProfiles) => set({ uploadProfiles }),
  setGlobalMaxFps: (fps) => set({ globalMaxFps: fps > 0 ? fps : 25 }),
  setDirty: (d) => set({ dirty: d }),
  loadAssets: async (name) => {
    try {
      const assets = await fetchAssets(name)
      set({ assets })
    } catch {
      set({ assets: EMPTY_ASSETS })
    }
  },
  loadUploadProfiles: async (name) => {
    try {
      const config = await fetchUploadConfig(name)
      set({ uploadProfiles: config.profiles ?? {} })
    } catch {
      set({ uploadProfiles: {} })
    }
  },
}))
