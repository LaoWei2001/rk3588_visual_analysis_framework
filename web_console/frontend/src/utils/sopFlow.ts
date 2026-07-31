// SOP 流程的数据模型 + 与 logic_parameters.flow 的互转。
// 单独抽一处, 让"画布节点 / 流程弹窗 / 图<->配置 序列化"都复用同一套定义, 互不耦合。

export interface SopStep {
  zoneName: string       // 引用的 ROI 区域名(来自上游 ROI 节点; 可重复 = 多次进入)
  enter_sec: number      // 该步进入区域确认时长(秒)
  dwell_min_sec: number  // 该步要求的最小停留(秒), 0 = 不要求
  dwell_max_sec: number  // 该步允许的最大停留(秒), 0 = 不限(可忽略)
  x?: number             // SOP 子画布上的 x 坐标(可选, 缺省 = 默认网格)
  y?: number             // SOP 子画布上的 y 坐标(可选, 缺省 = 默认网格)
}

// 工序结束判定方式: 离场超时 / 进入终点区域 / 外部触发
export type SopEndMode = 'leave' | 'endzone' | 'trigger'

// SOP 节点 data 的形状(一个 SOP 通道的完整流程定义, 含「结束判定」)
// DAG 多分支: steps 是节点定义清单(索引基于位置), edges 是有向边列表 [srcIdx,dstIdx]。
export type SopTriggerMode = 'auto' | 'external'

export interface SopFlow {
  target_label: string         // 跟踪的目标类别(labels.txt 里的类名)
  reset_sec: number            // 离场超时(秒)
  end_mode: SopEndMode         // 工序结束判定方式
  end_zone: string             // 终点区域名(end_mode='endzone' 时用)
  end_dwell_sec: number        // 终点区域连续停留达到此时长后结束; 0 = 进入确认后立即结束
  total_min_sec: number        // 工序总耗时下限(秒); 0 = 不限
  total_max_sec: number        // 工序总耗时上限(秒); 0 = 不限
  trigger_mode: SopTriggerMode  // 起点触发方式: "auto"=目标进入即开始; "external"=等待sop_trigger信号
  trigger_mandatory: boolean    // 仅trigger_mode=external时有效: true=未触发却进入区域则报违规
  report_normal: boolean        // 一轮正式结算且完全合规时，是否上报正常工序结果
  steps: SopStep[]             // 步骤定义(索引 = 位置, 不再代表"顺序")
  edges: [number, number][]    // 图边: 每条 [src_idx, dst_idx]; 允许自环/环
  edge_limits: Array<{ src: number; dst: number; min: number; max: number }>
  entries: number[]            // 起点 step 索引，必须由起点连线显式指定
  exits: number[]              // 出口 step 索引，必须由结束判定连线显式指定
  end_x?: number               // 结束判定节点在子画布上的 x 坐标
  end_y?: number               // 结束判定节点在子画布上的 y 坐标
}

export const DEFAULT_STEP_ENTER = 0.5
export const DEFAULT_STEP_DWELL = 0
export const DEFAULT_STEP_DWELL_MAX = 0   // 0 = 不限最大停留
export const DEFAULT_SOP_FLOW: SopFlow = {
  target_label: '', reset_sec: 5, end_mode: 'leave', end_zone: '', end_dwell_sec: 0,
  total_min_sec: 0, total_max_sec: 0, trigger_mode: 'auto', trigger_mandatory: false,
  report_normal: false, steps: [], edges: [], edge_limits: [], entries: [], exits: [],
}

const toNum = (s: unknown, dflt: number): number => {
  const t = String(s ?? '').trim()
  const v = Number(t)
  return t !== '' && Number.isFinite(v) ? v : dflt
}

const objectValue = (value: unknown): Record<string, unknown> =>
  value != null && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, unknown> : {}

function normalizeSopFlow(value: unknown): SopFlow {
  const raw = objectValue(value)
  const rawSteps = Array.isArray(raw.steps) ? raw.steps : []
  const steps: SopStep[] = rawSteps.map(item => {
    const step = objectValue(item)
    const x = Number(step.x)
    const y = Number(step.y)
    return {
      zoneName: String(step.zoneName ?? '').trim(),
      enter_sec: Math.max(0, toNum(step.enter_sec, DEFAULT_STEP_ENTER)),
      dwell_min_sec: Math.max(0, toNum(step.dwell_min_sec, DEFAULT_STEP_DWELL)),
      dwell_max_sec: Math.max(0, toNum(step.dwell_max_sec, DEFAULT_STEP_DWELL_MAX)),
      ...(Number.isFinite(x) ? { x } : {}),
      ...(Number.isFinite(y) ? { y } : {}),
    }
  })
  const validIndex = (value: unknown): value is number =>
    Number.isInteger(value) && Number(value) >= 0 && Number(value) < steps.length
  const edges: [number, number][] = (Array.isArray(raw.edges) ? raw.edges : [])
    .filter((item): item is [number, number] =>
      Array.isArray(item) && item.length === 2 && validIndex(item[0]) && validIndex(item[1]))
    .map(([src, dst]) => [Number(src), Number(dst)])
  const indices = (value: unknown): number[] =>
    Array.from(new Set((Array.isArray(value) ? value : []).filter(validIndex).map(Number)))
  const edge_limits = (Array.isArray(raw.edge_limits) ? raw.edge_limits : [])
    .map(objectValue)
    .filter(limit => validIndex(limit.src) && validIndex(limit.dst))
    .map(limit => ({
      src: Number(limit.src),
      dst: Number(limit.dst),
      min: Math.max(0, Math.round(toNum(limit.min, 0))),
      max: Math.max(0, Math.round(toNum(limit.max, 0))),
    }))
    .filter(limit => limit.min > 0 || limit.max > 0)
  const endX = Number(raw.end_x)
  const endY = Number(raw.end_y)
  return {
    target_label: String(raw.target_label ?? '').trim(),
    reset_sec: Math.max(0, toNum(raw.reset_sec, 5)),
    end_mode: raw.end_mode === 'endzone' || raw.end_mode === 'trigger' ? raw.end_mode : 'leave',
    end_zone: String(raw.end_zone ?? '').trim(),
    end_dwell_sec: Math.max(0, toNum(raw.end_dwell_sec, 0)),
    total_min_sec: Math.max(0, toNum(raw.total_min_sec, 0)),
    total_max_sec: Math.max(0, toNum(raw.total_max_sec, 0)),
    trigger_mode: raw.trigger_mode === 'external' ? 'external' : 'auto',
    trigger_mandatory: raw.trigger_mandatory === true,
    report_normal: raw.report_normal === true,
    steps,
    edges,
    edge_limits,
    entries: indices(raw.entries),
    exits: indices(raw.exits),
    ...(Number.isFinite(endX) ? { end_x: endX } : {}),
    ...(Number.isFinite(endY) ? { end_y: endY } : {}),
  }
}

export function sopFlowToParameters(flow: SopFlow): Record<string, unknown> {
  return { flow: normalizeSopFlow(flow) }
}

export function sopParametersToFlow(parameters: Record<string, unknown>): SopFlow {
  return normalizeSopFlow(parameters.flow)
}
