// SOP 流程的数据模型 + 与 config.json 字段的互转。
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
// edges 空 = 默认线性链(0→1→...→N-1), 完全兼容老配置。
export type SopTriggerMode = 'auto' | 'external'

export interface SopFlow {
  target_label: string         // 跟踪的目标类别(labels.txt 里的类名)
  reset_sec: number            // 离场超时(秒)
  end_mode: SopEndMode         // 工序结束判定方式
  end_zone: string             // 终点区域名(end_mode='endzone' 时用)
  end_dwell_sec: number        // 终点区域连续停留达到此时长后结束; 0 = 进入确认后立即结束
  total_min_sec: number        // 工序总耗时下限(秒); 0 = 不限
  total_max_sec: number        // 工序总耗时上限(秒); 0 = 不限
  trigger_mode?: SopTriggerMode // 起点触发方式: "auto"(默认)=目标进入即开始; "external"=等待sop_trigger信号
  trigger_mandatory?: boolean    // 仅trigger_mode=external时有效: true=未触发却进入区域则报违规
  report_normal?: boolean        // 一轮正式结算且完全合规时，是否上报正常工序结果
  steps: SopStep[]             // 步骤定义(索引 = 位置, 不再代表"顺序")
  edges?: [number, number][]   // 图边: 每条 [src_idx, dst_idx]; 允许自环/环; 缺省/空 = 线性链
  edge_limits?: Array<{ src: number; dst: number; min: number; max: number }>  // 边循环次数约束(min/max=0 表示该侧不限; 两端都 0 = 该边无约束)
  entries?: number[]           // 起点 step 索引: 被标记为「🚩 起点」的步骤(一条路线的第一步);
                               // 多个 = 多条可选路线(可同区域, 靠后续区域区分); 缺省/空 → 后端 fallback step 0
  exits?: number[]             // 出口 step 索引: 用户连到「🏁 结束判定」的 source step;
                               // 漏检判定: visited 子图必须存在 entry→exit 路径才算合规;
                               // 缺省/空 → 后端 fallback 到"出度0"或"全 visited"
  end_x?: number               // 结束判定节点在子画布上的 x 坐标
  end_y?: number               // 结束判定节点在子画布上的 y 坐标
}

export const DEFAULT_STEP_ENTER = 0.5
export const DEFAULT_STEP_DWELL = 0
export const DEFAULT_STEP_DWELL_MAX = 0   // 0 = 不限最大停留
export const DEFAULT_SOP_FLOW: SopFlow = {
  target_label: '', reset_sec: 5, end_mode: 'leave', end_zone: '', end_dwell_sec: 0,
  total_min_sec: 0, total_max_sec: 0, report_normal: false, steps: [], edges: [], exits: [],
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
  const edges = f.edges ?? []
  // 边列表 → "src-dst,src-dst,..."; 空数组 → 空串(C++ 端将回退到线性链)
  // 允许 a===b 自环(C++ path_edges 解析会忽略 a==b, 但前端要靠它把自环线存盘/还原)
  const path_edges = edges
    .filter(([a, b]) => a >= 0 && a < steps.length && b >= 0 && b < steps.length)
    .map(([a, b]) => `${a}-${b}`)
    .join(',')
  // 画布坐标(子画布的"_editor_layout"): 与 path_sequence 按位置对齐
  const hasLayout = steps.some(s => s.x != null || s.y != null)
  const path_step_x_list = hasLayout ? steps.map(s => Math.round(Number(s.x ?? 0))).join(',') : ''
  const path_step_y_list = hasLayout ? steps.map(s => Math.round(Number(s.y ?? 0))).join(',') : ''
  // 起点集合 → "0,2" 字符串(去重/过滤越界)
  const entrySet = Array.from(new Set((f.entries ?? []).filter(i => i >= 0 && i < steps.length)))
  const path_entries = entrySet.join(',')
  // 出口集合 → "0,3,5" 字符串(去重/过滤越界)
  const exitSet = Array.from(new Set((f.exits ?? []).filter(i => i >= 0 && i < steps.length)))
  const path_exits = exitSet.join(',')
  // 边循环次数约束 → "src-dst:min-max,..."(只输出 min/max 至少一边 > 0 的)
  const path_edge_limits = (f.edge_limits ?? [])
    .filter(l => l.src >= 0 && l.src < steps.length && l.dst >= 0 && l.dst < steps.length
                  && ((l.min ?? 0) > 0 || (l.max ?? 0) > 0))
    .map(l => `${l.src}-${l.dst}:${Math.max(0, Math.round(l.min ?? 0))}-${Math.max(0, Math.round(l.max ?? 0))}`)
    .join(',')
  return {
    logic: 'logic_path_sop',
    path_target_label: (f.target_label ?? '').trim(),
    path_reset_sec: Number(f.reset_sec ?? 5),
    path_end_mode: f.end_mode === 'endzone' ? 'endzone' : (f.end_mode === 'trigger' ? 'trigger' : 'leave'),
    path_end_zone: (f.end_zone ?? '').trim(),
    path_end_dwell_sec: Math.max(0, Number(f.end_dwell_sec ?? 0)),
    path_total_min_sec: Number(f.total_min_sec ?? 0),
    path_total_max_sec: Number(f.total_max_sec ?? 0),
    path_trigger_mode: f.trigger_mode === 'external' ? 'external' : 'auto',
    path_trigger_mandatory: f.trigger_mandatory === true,
    path_report_normal: f.report_normal === true,
    path_sequence:  steps.map(s => s.zoneName.trim()).join(','),
    path_enter_list: steps.map(s => Number(s.enter_sec ?? DEFAULT_STEP_ENTER)).join(','),
    path_dwell_list: steps.map(s => Number(s.dwell_min_sec ?? DEFAULT_STEP_DWELL)).join(','),
    path_dwell_max_list: steps.map(s => Number(s.dwell_max_sec ?? DEFAULT_STEP_DWELL_MAX)).join(','),
    path_edges,
    path_entries,
    path_exits,
    path_edge_limits,
    path_step_x_list,
    path_step_y_list,
    path_end_x: Math.round(Number(f.end_x ?? 0)),
    path_end_y: Math.round(Number(f.end_y ?? 0)),
  }
}

// logic_path_sop 的通道字段(path_*) → SopFlow。
export function sopConfigToFlow(ch: Record<string, unknown>): SopFlow {
  const seq    = String(ch.path_sequence ?? '').split(',').map(s => s.trim()).filter(Boolean)
  const enterL = String(ch.path_enter_list ?? '').split(',')
  const dwellL = String(ch.path_dwell_list ?? '').split(',')
  const maxL   = String(ch.path_dwell_max_list ?? '').split(',')
  const xL     = String(ch.path_step_x_list ?? '').split(',')
  const yL     = String(ch.path_step_y_list ?? '').split(',')
  const eDflt  = toNum(ch.path_enter_sec, DEFAULT_STEP_ENTER)
  const dDflt  = toNum(ch.path_dwell_min_sec, DEFAULT_STEP_DWELL)
  const mDflt  = toNum(ch.path_dwell_max_sec, DEFAULT_STEP_DWELL_MAX)
  const steps: SopStep[] = seq.map((zoneName, i) => {
    // 坐标: 拿不到则不填(SopFlowModal 初始化时回退到默认网格)
    const xv = i < xL.length && xL[i].trim() !== '' ? Number(xL[i]) : undefined
    const yv = i < yL.length && yL[i].trim() !== '' ? Number(yL[i]) : undefined
    return {
      zoneName,
      enter_sec:     i < enterL.length ? toNum(enterL[i], eDflt) : eDflt,
      dwell_min_sec: i < dwellL.length ? toNum(dwellL[i], dDflt) : dDflt,
      dwell_max_sec: i < maxL.length   ? toNum(maxL[i],   mDflt) : mDflt,
      ...(Number.isFinite(xv) ? { x: xv as number } : {}),
      ...(Number.isFinite(yv) ? { y: yv as number } : {}),
    }
  })
  // 边列表反解析: "0-1,0-3" → [[0,1],[0,3]]; 空串/不合法 → 空数组(渲染时回退到线性链可视化)
  const edges: [number, number][] = String(ch.path_edges ?? '')
    .split(',')
    .map(s => s.trim())
    .filter(Boolean)
    .map(tok => tok.split('-').map(n => Number(n.trim())) as [number, number])
    .filter(([a, b]) =>   // 允许 a===b 自环(还原自环线)
      Number.isFinite(a) && Number.isFinite(b) && a >= 0 && a < steps.length && b >= 0 && b < steps.length)
  // 起点集合反解析: "0,2" → [0, 2]
  const entries: number[] = Array.from(new Set(String(ch.path_entries ?? '')
    .split(',')
    .map(s => s.trim())
    .filter(Boolean)
    .map(s => Number(s))
    .filter(n => Number.isFinite(n) && n >= 0 && n < steps.length)))
  // 出口集合反解析: "0,3" → [0, 3]
  const exits: number[] = Array.from(new Set(String(ch.path_exits ?? '')
    .split(',')
    .map(s => s.trim())
    .filter(Boolean)
    .map(s => Number(s))
    .filter(n => Number.isFinite(n) && n >= 0 && n < steps.length)))
  // 边循环次数约束反解析: "1-0:2-5,1-2:0-3" → [{src:1,dst:0,min:2,max:5}, {src:1,dst:2,min:0,max:3}]
  const edge_limits = String(ch.path_edge_limits ?? '')
    .split(',')
    .map(s => s.trim())
    .filter(Boolean)
    .map(tok => {
      const m = tok.match(/^(\d+)-(\d+):(\d+)-(\d+)$/)
      if (!m) return null
      const src = Number(m[1]), dst = Number(m[2]), min = Number(m[3]), max = Number(m[4])
      if (!Number.isFinite(src) || !Number.isFinite(dst) || src >= steps.length || dst >= steps.length) return null
      return { src, dst, min, max }
    })
    .filter((x): x is { src: number; dst: number; min: number; max: number } => !!x)
  // 结束判定节点坐标
  const endX = Number(ch.path_end_x ?? NaN)
  const endY = Number(ch.path_end_y ?? NaN)
  return {
    target_label: String(ch.path_target_label ?? ''),
    reset_sec: Number(ch.path_reset_sec ?? 5),
    end_mode: (ch.path_end_mode === 'endzone' || ch.path_end_mode === 'trigger') ? ch.path_end_mode as SopEndMode : 'leave',
    end_zone: String(ch.path_end_zone ?? ''),
    end_dwell_sec: Math.max(0, Number(ch.path_end_dwell_sec ?? 0)),
    total_min_sec: Number(ch.path_total_min_sec ?? 0),
    total_max_sec: Number(ch.path_total_max_sec ?? 0),
    trigger_mode: ch.path_trigger_mode === 'external' ? 'external' : 'auto',
    trigger_mandatory: ch.path_trigger_mandatory === true || ch.path_trigger_mandatory === 'true' || ch.path_trigger_mandatory === 1,
    report_normal: ch.path_report_normal === true || ch.path_report_normal === 'true' || ch.path_report_normal === 1,
    steps,
    edges,
    entries,
    exits,
    edge_limits,
    ...(Number.isFinite(endX) ? { end_x: endX } : {}),
    ...(Number.isFinite(endY) ? { end_y: endY } : {}),
  }
}
