// SOP 流程的数据模型 + 与 config.json 字段的互转。
// 单独抽一处, 让"画布节点 / 流程弹窗 / 图<->配置 序列化"都复用同一套定义, 互不耦合。

export interface SopStep {
  zoneName: string       // 引用的 ROI 区域名(来自上游 ROI 节点; 可重复 = 多次进入)
  enter_sec: number      // 该步进入区域确认时长(秒)
  dwell_min_sec: number  // 该步要求的最小停留(秒), 0 = 不要求
}

// 工序结束判定方式: 离场超时 / 进入终点区域
export type SopEndMode = 'leave' | 'endzone'

// SOP 节点 data 的形状(一个 SOP 通道的完整流程定义)
export interface SopFlow {
  target_label: string   // 跟踪的目标类别(labels.txt 里的类名)
  reset_sec: number      // 离场超时(秒): 目标离场持续此久 → 工序结束(leave 主判定 / endzone 兜底)
  end_mode: SopEndMode   // 工序结束判定方式
  end_zone: string       // 终点区域名(end_mode='endzone' 时用)
  steps: SopStep[]       // 有序步骤
}

export const DEFAULT_STEP_ENTER = 0.5
export const DEFAULT_STEP_DWELL = 0
export const DEFAULT_SOP_FLOW: SopFlow = {
  target_label: '', reset_sec: 5, end_mode: 'leave', end_zone: '', steps: [],
}

const toNum = (s: unknown, dflt: number): number => {
  const t = String(s ?? '').trim()
  const v = Number(t)
  return t !== '' && Number.isFinite(v) ? v : dflt
}

// SopFlow → logic_path_sop 的通道字段(path_*)。
// path_enter_list / path_dwell_list 与 path_sequence 按位置对齐, 因此每步参数各自独立。
export function sopFlowToConfig(f: SopFlow): Record<string, unknown> {
  const steps = f.steps ?? []
  return {
    logic: 'logic_path_sop',
    path_target_label: (f.target_label ?? '').trim(),
    path_reset_sec: Number(f.reset_sec ?? 5),
    path_end_mode: f.end_mode === 'endzone' ? 'endzone' : 'leave',
    path_end_zone: (f.end_zone ?? '').trim(),
    path_sequence:  steps.map(s => s.zoneName.trim()).join(','),
    path_enter_list: steps.map(s => Number(s.enter_sec ?? DEFAULT_STEP_ENTER)).join(','),
    path_dwell_list: steps.map(s => Number(s.dwell_min_sec ?? DEFAULT_STEP_DWELL)).join(','),
  }
}

// logic_path_sop 的通道字段(path_*) → SopFlow。
export function sopConfigToFlow(ch: Record<string, unknown>): SopFlow {
  const seq    = String(ch.path_sequence ?? '').split(',').map(s => s.trim()).filter(Boolean)
  const enterL = String(ch.path_enter_list ?? '').split(',')
  const dwellL = String(ch.path_dwell_list ?? '').split(',')
  const eDflt  = toNum(ch.path_enter_sec, DEFAULT_STEP_ENTER)
  const dDflt  = toNum(ch.path_dwell_min_sec, DEFAULT_STEP_DWELL)
  const steps: SopStep[] = seq.map((zoneName, i) => ({
    zoneName,
    enter_sec:     i < enterL.length ? toNum(enterL[i], eDflt) : eDflt,
    dwell_min_sec: i < dwellL.length ? toNum(dwellL[i], dDflt) : dDflt,
  }))
  return {
    target_label: String(ch.path_target_label ?? ''),
    reset_sec: Number(ch.path_reset_sec ?? 5),
    end_mode: ch.path_end_mode === 'endzone' ? 'endzone' : 'leave',
    end_zone: String(ch.path_end_zone ?? ''),
    steps,
  }
}
