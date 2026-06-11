import { create } from 'zustand'

// 一个 ROI 区域: 名字(可空) + 多边形(归一化 0~1, 占画面比例, 闭合=首尾点相同)。
// 一个通道(=一个 ROI 节点)可以有多个区域。
export interface Zone {
  name: string
  polygon: number[][]
}

interface ROIState {
  // key: React Flow node ID → 该节点(通道)的全部 ROI 区域
  zones: Record<string, Zone[]>
  // key: React Flow node ID → 抓帧时的源分辨率 [w,h]（仅作宽高比/信息, 不参与坐标换算）
  resolutions: Record<string, [number, number]>

  setZones: (nodeId: string, zones: Zone[], srcW?: number, srcH?: number) => void
  clearZones: (nodeId: string) => void
  setAll: (data: Record<string, Zone[]>) => void
  clear: () => void
}

export const useROIStore = create<ROIState>((set) => ({
  zones: {},
  resolutions: {},

  setZones: (nodeId, zones, srcW, srcH) =>
    set(s => ({
      zones: { ...s.zones, [nodeId]: zones },
      resolutions: (srcW && srcH)
        ? { ...s.resolutions, [nodeId]: [srcW, srcH] }
        : s.resolutions,
    })),

  clearZones: (nodeId) =>
    set(s => {
      const nextZ = { ...s.zones }
      const nextR = { ...s.resolutions }
      delete nextZ[nodeId]
      delete nextR[nodeId]
      return { zones: nextZ, resolutions: nextR }
    }),

  // 加载/导入配置时整体替换 ROI：zones 用新配置的，resolutions 一并清空，
  // 避免上一份配置的分辨率按确定性节点 ID 串用到新配置。
  setAll: (data) => set({ zones: data, resolutions: {} }),
  clear: () => set({ zones: {}, resolutions: {} }),
}))
