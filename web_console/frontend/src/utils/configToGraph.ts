import { Node, Edge, MarkerType } from '@xyflow/react'
import type { GlobalLogicEntry } from '../components/GlobalLogicsPanel'
import type { GlobalSettingsData } from '../components/GlobalSettingsPanel'
import { DEFAULT_GLOBAL_SETTINGS } from '../components/GlobalSettingsPanel'
import type { RoiZone } from './graphToConfig'

// NOTE: _ctr is intentionally declared inside configToGraph() below
//       so IDs are deterministic (reset each call) — required for position restore.

const DIFY_LOGICS   = ['logic_dify', 'logic_dify_person_verify']
const SERVER_LOGICS = ['logic_server']

// 优先 logics.json 的 report 字段，回退内置名单（与 graphToConfig 对称）。
function resolveReport(
  logic: string,
  reportByLogic?: Record<string, string>,
): 'server' | 'dify' | null {
  const r = reportByLogic?.[logic]
  if (r === 'server' || r === 'dify') return r
  if (DIFY_LOGICS.includes(logic)) return 'dify'
  if (SERVER_LOGICS.includes(logic)) return 'server'
  return null
}

// 通道里若带上报字段(server_url / dify_*) → 说明画布上连过报警节点, 反序列化时据此重建,
// 与 logic 是否声明 report 解耦(修复: 连到未声明 report 的 logic 上的报警节点, 刷新后消失)。
function channelReportType(ch: Record<string, unknown>): 'server' | 'dify' | null {
  if ('dify_prompt' in ch || 'dify_api_url' in ch || 'dify_api_key' in ch) return 'dify'
  if ('server_url' in ch) return 'server'
  return null
}

// 模型节点字段；通道里除这些(及 stream/logic/dify_prompt/server_url)之外的键，一律视为 logic 参数，路由到逻辑节点
const MODEL_KEYS = new Set([
  'id', 'enable', 'infer_enable', 'npu_core', 'model_type', 'model_path', 'label_path',
  'obj_thresh', 'nms_thresh', 'detect_classes', 'threads', 'max_fps',
  'playback_fps', 'tracker_enable', 'tracker_iou_thresh', 'tracker_max_miss',
  'tracker_min_hits', 'version', 'roi_polygon', 'roi_zones',
])


export function configToGraph(
  config: Record<string, unknown>,
  roi: Record<string, { polygon?: number[][]; zones?: RoiZone[] }>,
  reportByLogic: Record<string, string> = {}
): {
  nodes: Node[]
  edges: Edge[]
  roiMapping: Record<string, RoiZone[]>
  globalLogics: GlobalLogicEntry[]
  globalSettings: GlobalSettingsData
} {
  // Deterministic IDs: reset counter each call so the same config always
  // produces the same node IDs — this is required for localStorage position restore.
  let _ctr = 0
  const uid = (p: string) => `${p}-${++_ctr}`

  // edge helper lives here so it can close over uid
  const edge = (
    source: string, sourceHandle: string,
    target: string, targetHandle: string,
    color: string
  ): Edge => ({
    id: uid('e'),
    source, sourceHandle,
    target, targetHandle,
    type: 'default',
    style: { stroke: color, strokeWidth: 1.5 },
    markerEnd: { type: MarkerType.ArrowClosed, color },
  })

  const nodes: Node[] = []
  const edges: Edge[] = []
  const roiMapping: Record<string, RoiZone[]> = {}

  const global   = (config.global ?? config) as Record<string, unknown>
  const channels = (config.channels as Record<string, unknown>[]) ?? []

  // Global logics
  const rawGL = (global.global_logics as Record<string, unknown>[]) ?? []
  const globalLogics: GlobalLogicEntry[] = rawGL.map(gl => ({
    enable:           (gl.enable          as boolean) ?? true,
    logic:            (gl.logic           as string)  ?? 'global_default',
    channels:         (gl.channels        as number[]) ?? [],
    poll_interval_ms: (gl.poll_interval_ms as number) ?? 200,
  }))

  const { global_logics: _gl, ...rawGD } = global
  const globalSettings: GlobalSettingsData = { ...DEFAULT_GLOBAL_SETTINGS, ...rawGD }

  // Layout constants
  const STREAM_X  = 60
  const MODEL_X   = 380
  const ROI_X     = 220  // above model
  const LOGIC_X   = 680
  const REPORT_X  = 940
  const ROW_H     = 260  // vertical spacing per channel group

  channels.forEach((ch, idx) => {
    const y      = idx * ROW_H + 60
    const origId = (ch.id as number) ?? idx
    const stream = ch.stream as Record<string, unknown> ?? {}

    // 该通道是否带 YOLO 模型节点: 配了 model_type 或 model_path → 画 model 节点
    // (含“启用推理”关掉但仍配了模型的 YOLO 通道); 两者皆空 → 传统/无推理通道:
    // 视频流直连逻辑函数, 不画 model 节点。graphToConfig 对传统通道刻意不写这两个字段。
    const mType = typeof ch.model_type === 'string' ? (ch.model_type as string).trim() : ''
    const mPath = typeof ch.model_path === 'string' ? (ch.model_path as string).trim() : ''
    const hasModel = mType !== '' || mPath !== ''

    // ── Stream node — 每个通道独立创建，不做去重 ──
    // 通道号 (channel_id) 唯一，一个 StreamNode 只能连一个下游节点，
    // 因此即使两个通道 URL 相同，也必须各自有独立的 StreamNode。
    const streamId = uid('stream')
    nodes.push({
      id: streamId, type: 'stream',
      position: { x: STREAM_X, y },
      data: { ...stream, channel_id: origId },
    })

    // ── 通道字段分流：模型字段 → 模型节点；其余(radius 及任意 logic 参数) → 逻辑节点 ──
    const { stream: _s, logic: _lg, dify_prompt: _dp, server_url: _su,
            dify_api_url: _du, dify_api_key: _dk, ...rest } = ch
    const modelData:   Record<string, unknown> = {}
    const logicParams: Record<string, unknown> = {}
    Object.entries(rest).forEach(([k, v]) => {
      if (MODEL_KEYS.has(k)) modelData[k] = v
      else                   logicParams[k] = v
    })

    // ── Model node (仅 YOLO 通道) ──
    // 节点创建顺序保持 stream→model→roi→logic→report, 使同一份配置始终生成相同节点 ID
    // (localStorage 画布布局恢复依赖确定性 ID)。
    let modelId: string | null = null
    if (hasModel) {
      modelId = uid('model')
      nodes.push({
        id: modelId, type: 'model',
        position: { x: MODEL_X, y },
        data: { ...modelData },
      })
      edges.push(edge(streamId, 'stream-out', modelId, 'stream-in', '#3b82f6'))
    }

    // ── ROI node (一个 ROI 节点 = 该通道的多个命名区域) ──
    // Use sequential position (idx) as key — must match C++ load_roi_zones() which iterates
    // channels by sorted position (ch=0,1,2…), NOT by channel id.
    // 来源优先级: roi_zones.json 的 zones[] → 旧 polygon → 内嵌 ch.roi_zones → 内嵌 ch.roi_polygon。
    const roiEntry = roi[String(idx)]
    let zones: RoiZone[] = []
    if (roiEntry?.zones && roiEntry.zones.length > 0) {
      zones = roiEntry.zones
    } else if (roiEntry?.polygon && roiEntry.polygon.length >= 3) {
      zones = [{ name: '', polygon: roiEntry.polygon }]
    } else if (Array.isArray(ch.roi_zones) && (ch.roi_zones as RoiZone[]).length > 0) {
      zones = ch.roi_zones as RoiZone[]
    } else if (Array.isArray(ch.roi_polygon) && (ch.roi_polygon as number[][]).length >= 3) {
      zones = [{ name: '', polygon: ch.roi_polygon as number[][] }]
    }
    zones = zones.filter(z => Array.isArray(z.polygon) && z.polygon.length >= 3)
    let roiId: string | null = null
    if (zones.length > 0) {
      roiId = uid('roi')
      nodes.push({
        id: roiId, type: 'roi',
        position: { x: ROI_X, y: y - 80 },
        data: {},
      })
      roiMapping[roiId] = zones
      // YOLO 通道此处即连 model; 传统通道的 roi→logic 连线在 logic 节点创建后补。
      if (hasModel) edges.push(edge(roiId, 'roi-out', modelId!, 'roi-in', '#f97316'))
    }

    // ── Logic node ──
    const logic   = String(_lg ?? 'logic_default')
    const logicId = uid('logic')
    // logic 名 + 该通道的所有 logic 参数(radius 等)；具体渲染哪些由 logics.json 决定
    const logicData: Record<string, unknown> = { logic, ...logicParams }
    nodes.push({
      id: logicId, type: 'logic',
      position: { x: hasModel ? LOGIC_X : MODEL_X, y },  // 传统通道 logic 占据 model 的列位置, 更紧凑
      data: logicData,
    })
    if (hasModel) {
      edges.push(edge(modelId!, 'logic-out', logicId, 'logic-in', '#a855f7'))
    } else {
      // 传统/无推理通道: 视频流直连逻辑函数; 有 ROI 则 ROI 接到逻辑函数顶部
      edges.push(edge(streamId, 'stream-out', logicId, 'logic-in', '#3b82f6'))
      if (roiId) edges.push(edge(roiId, 'roi-out', logicId, 'roi-in', '#f97316'))
    }

    // ── Report node (only for logics that need it) ──
    const reportType = resolveReport(logic, reportByLogic) ?? channelReportType(ch)
    if (reportType) {
      const reportId   = uid('report')
      const reportData: Record<string, unknown> = {}
      if (reportType === 'dify') {
        reportData.report_type  = 'dify'
        reportData.dify_prompt  = _dp ?? ''
        reportData.dify_api_url = _du ?? ''
        reportData.dify_api_key = _dk ?? ''
      } else {
        reportData.report_type = 'server'
        reportData.server_url  = _su ?? ''
      }
      nodes.push({
        id: reportId, type: 'report',
        position: { x: hasModel ? REPORT_X : LOGIC_X, y },
        data: reportData,
      })
      edges.push(edge(logicId, 'report-out', reportId, 'report-in', '#ef4444'))
    }
  })

  return { nodes, edges, roiMapping, globalLogics, globalSettings }
}
