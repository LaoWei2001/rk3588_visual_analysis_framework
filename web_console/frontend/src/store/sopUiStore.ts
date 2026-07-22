import { create } from 'zustand'

// 极小的 UI 协调标志: SOP 流程弹窗是否正打开。
// 用途: 弹窗打开时, 主画布的 ReactFlow 要暂停"Delete 删节点"——否则按 Delete 删的是
// 主画布上选中的 SOP 节点, 而不是弹窗子画布里的步骤。弹窗自己的 ReactFlow 照常响应 Delete。
interface SopUiState {
  flowOpen: boolean
  setFlowOpen: (open: boolean) => void
}

export const useSopUiStore = create<SopUiState>((set) => ({
  flowOpen: false,
  setFlowOpen: (open) => set({ flowOpen: open }),
}))
