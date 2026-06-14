import { Node, Edge } from '@xyflow/react'
import { getSrcType } from './streamSource'
import type { GlobalLogicEntry } from '../components/GlobalLogicsPanel'
import type { GlobalSettingsData } from '../components/GlobalSettingsPanel'
import type { Zone } from '../store/roiStore'

// 一个通道可有多个 ROI 区域: 区域名 + 多边形(归一化坐标)。(与 roiStore 的 Zone 同构)
export type RoiZone = Zone
// roi_zones.json 的 per-channel 结构: polygon=首个区域(向后兼容), zones=全部区域。
type RoiEntry = { polygon: number[][]; zones: RoiZone[] }

// 内置回退名单（logics.json 不可用时用）。优先读 logics.json 的 report 字段，见 resolveReport。
const DIFY_LOGICS   = ['logic_dify', 'logic_dify_person_verify']
const SERVER_LOGICS = ['logic_server']

// 解析某 logic 的上报类型：优先 logics.json 的 report 字段(reportByLogic)，回退内置名单。
// 开发者在 channel_logic.cpp 新写的逻辑，只要在 logics.json 声明 "report":"server"/"dify"，
// 网页就自动给它显示上报配置、并把地址/提示词写进 config.json —— 前端无需改。
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

function buildStream(d: Record<string, unknown>): Record<string, unknown> {
  // src_type 必填、不再自动推断；按【显式】类型分别落字段（新建节点已带 src_type）。
  const t = getSrcType(d)
  if (t === 'usb') {
    const s: Record<string, unknown> = { src_type: 'usb', device: d.device ?? '/dev/video0' }
    // 方案B: 显式 USB 采集分辨率(0=自动随 fps)。写进 config 供 C++ 与 ROI 抓帧用同一值
    const uw = Number(d.usb_width ?? 0), uh = Number(d.usb_height ?? 0)
    if (uw > 0 && uh > 0) { s.usb_width = uw; s.usb_height = uh }
    return s
  }
  if (t === 'file') return { src_type: 'file', url: d.url ?? '', loop: d.loop ?? true }
  if (t === 'rtsp') return { src_type: 'rtsp', url: d.url ?? '', video_enc: d.video_enc ?? 'h264' }
  // 未指定 src_type：原样写出 url，但保留空 src_type → 后端配置校验会拒绝，提示用户补全
  return { src_type: '', url: d.url ?? '' }
}

export function graphToConfig(
  nodes: Node[],
  edges: Edge[],
  roiZones: Record<string, Zone[]>,
  globalLogics: GlobalLogicEntry[] = [],
  globalSettings: GlobalSettingsData,
  reportByLogic: Record<string, string> = {}
): { config: Record<string, unknown>; roi: Record<string, RoiEntry> } | null {
  // ── 收集“通道锚点”: 一个锚点 = 一条通道 ──
  // YOLO 通道锚点 = model 节点; 传统/无推理通道锚点 = 被「视频流」直连的 logic 节点。
  // 用户可以不放 YOLO 节点, 把视频流直接接到逻辑函数 → 该路走传统 CV / 非 YOLO 算法。
  const isStreamFedLogic = (n: Node): boolean => {
    if (n.type !== 'logic') return false
    const inEdge = edges.find(e => e.target === n.id && e.targetHandle === 'logic-in')
    if (!inEdge) return false
    return nodes.find(s => s.id === inEdge.source)?.type === 'stream'
  }

  type Anchor = { node: Node; isModel: boolean }
  const anchors: Anchor[] = [
    ...nodes.filter(n => n.type === 'model').map(node => ({ node, isModel: true })),
    ...nodes.filter(isStreamFedLogic).map(node => ({ node, isModel: false })),
  ]
  if (anchors.length === 0) return null

  // 统一按画布位置排序(主按 y 分行, 同一行再按 x)。序号 idx 即通道在 config.channels 中的位置,
  // 也是 ROI 的 key —— 必须与 C++ load_roi_zones() 的“按位置遍历(ch=0,1,2…)”对齐。
  anchors.sort((a, b) => {
    const dy = a.node.position.y - b.node.position.y
    return Math.abs(dy) > 20 ? dy : a.node.position.x - b.node.position.x
  })

  const channels: Record<string, unknown>[] = []
  const roiOut: Record<string, RoiEntry> = {}

  anchors.forEach(({ node: aNode, isModel }, idx) => {
    const m = isModel ? (aNode.data as Record<string, unknown>) : {}

    // ── Stream ── YOLO: 流接 model 的 stream-in; 传统: 流直连 logic 的 logic-in ──
    const streamEdge = isModel
      ? edges.find(e => e.target === aNode.id && e.targetHandle === 'stream-in')
      : edges.find(e => e.target === aNode.id && e.targetHandle === 'logic-in'
                        && nodes.find(s => s.id === e.source)?.type === 'stream')
    const streamNode = streamEdge ? nodes.find(n => n.id === streamEdge.source) : null
    const stream = streamNode ? buildStream(streamNode.data as Record<string, unknown>) : { url: '', video_enc: 'h264' }

    // ── Channel id: use stream's channel_id if set, else fall back to sorted position ──
    const chId = streamNode != null && (streamNode.data as Record<string, unknown>).channel_id != null
      ? Number((streamNode.data as Record<string, unknown>).channel_id)
      : idx

    // ── ROI(一个 ROI 节点 = 该通道的多个命名区域; model 与 logic 都用同一个 roi-in handle) ──
    // KEY MUST be sequential position (idx), NOT channel_id.
    // C++ load_roi_zones() iterates channels by sorted position (ch=0,1,2…) and looks up
    // key = std::to_string(ch). Using channel_id here would misplace ROI for non-sequential ids.
    const roiEdge = edges.find(e => e.target === aNode.id && e.targetHandle === 'roi-in')
    const zones: RoiZone[] = (roiEdge ? (roiZones[roiEdge.source] ?? []) : [])
      .filter(z => Array.isArray(z.polygon) && z.polygon.length >= 3)
      .map(z => ({ name: z.name ?? '', polygon: z.polygon }))
    const roiPoly = zones.length > 0 ? zones[0].polygon : []   // 首区域(向后兼容单 ROI)
    roiOut[String(idx)] = { polygon: roiPoly, zones }

    // ── Logic ── YOLO: model 的 logic-out → logic 节点; 传统: 锚点自身即 logic 节点 ──
    const logicNode = isModel
      ? (() => {
          const le = edges.find(e => e.source === aNode.id && e.sourceHandle === 'logic-out')
          return le ? nodes.find(n => n.id === le.target) ?? null : null
        })()
      : aNode
    const l = logicNode ? (logicNode.data as Record<string, unknown>) : {}
    const logic = String(l.logic ?? 'logic_default')

    // ── Report ──
    const reportEdge = logicNode
      ? edges.find(e => e.source === logicNode.id && e.sourceHandle === 'report-out')
      : null
    const reportNode = reportEdge ? nodes.find(n => n.id === reportEdge.target) : null
    const r = reportNode ? (reportNode.data as Record<string, unknown>) : {}

    // ── 通道基础字段 ── YOLO 与传统通道字段集不同 ──
    const ch: Record<string, unknown> = isModel
      ? {
          id:             chId,
          enable:         true,                       // 通道存在即启用；YOLO 节点的开关现在控制 infer_enable
          infer_enable:   m.infer_enable   ?? true,   // 推理开关：false=传统算法通道(仍解码/显示/逐帧跑逻辑)
          stream,
          npu_core:       m.npu_core       ?? 0,
          logic,
          model_type:     m.model_type     ?? 'yolov8_det',
          model_path:     m.model_path     ?? '',
          label_path:     m.label_path     ?? '',
          obj_thresh:     m.obj_thresh     ?? 0.3,
          nms_thresh:     m.nms_thresh     ?? 0.45,
          detect_classes: (m.detect_classes as string[]) ?? [],
        }
      : {
          // 传统/无推理通道: 不写任何模型字段。C++ 见 model_path 为空即跳过 NPU 推理,
          // 仍解码/显示/逐帧跑 logic(ctx->results 为空, ctx->infer_enabled=0)。
          // 缺省 model_type/model_path 也是 configToGraph 区分“无 YOLO 节点”的依据。
          id:           chId,
          enable:       true,
          infer_enable: false,
          stream,
          logic,
        }

    // 上报参数仍由「上报配置」节点提供
    // 方案2: 上报地址每通道独立写进 config.json(空=用上报服务默认值)，随后经 C++ → Redis 消息下发
    const reportType = resolveReport(logic, reportByLogic)
    if (reportType === 'dify') {
      ch.dify_prompt  = r.dify_prompt  ?? l.dify_prompt ?? ''
      ch.dify_api_url = r.dify_api_url ?? ''
      ch.dify_api_key = r.dify_api_key ?? ''
    }
    if (reportType === 'server') ch.server_url = r.server_url ?? ''
    // 逻辑节点上的参数(除 logic 名本身)→ 写入通道；由 logics.json 动态驱动，不再逐字段硬编码
    // (上报地址/提示词由上报节点负责，这里跳过以免重复)
    Object.entries(l).forEach(([k, v]) => {
      if (k === 'logic' || k === 'dify_prompt' || k === 'server_url'
          || k === 'dify_api_url' || k === 'dify_api_key') return
      if (v != null) ch[k] = v
    })

    // Per-channel tracker overrides (仅 YOLO 通道; 传统通道 m={} 自然跳过)
    if (m.tracker_enable   != null) ch.tracker_enable   = m.tracker_enable
    if (m.tracker_iou_thresh != null) ch.tracker_iou_thresh = m.tracker_iou_thresh
    if (m.tracker_max_miss != null) ch.tracker_max_miss = m.tracker_max_miss
    if (m.tracker_min_hits != null) ch.tracker_min_hits = m.tracker_min_hits
    if (m.threads          != null) ch.threads          = m.threads
    if (m.max_fps          != null) ch.max_fps          = m.max_fps

    // 把 ROI 也内嵌进 config.json, 供网页重新加载时 configToGraph 还原(C++ 不读这里, ROI 只认 roi_zones.json)。
    if (zones.length > 0) {
      ch.roi_zones   = zones    // 全部区域
      ch.roi_polygon = roiPoly  // 首区域(向后兼容)
    }

    channels.push(ch)
  })

  // ── Global config ──
  const g = globalSettings
  const globalCfg: Record<string, unknown> = {
    model_path: g.model_path ?? '',
    label_path: g.label_path ?? '',
    enable_display:     g.enable_display     ?? 0,
    disp_width:         g.disp_width         ?? 1920,
    disp_height:        g.disp_height        ?? 1080,
    tile_rows:          g.tile_rows          ?? 2,
    tile_cols:          g.tile_cols          ?? 2,
    max_fps:            g.max_fps            ?? 15,
    queue_size:         g.queue_size         ?? 1,
    channel_threads:    g.channel_threads    ?? 1,
    npu_cores:          g.npu_cores          ?? 3,
    obj_thresh:         g.obj_thresh         ?? 0.3,
    nms_thresh:         g.nms_thresh         ?? 0.45,
    detect_classes:     g.detect_classes     ?? [],
    tracker_enable:     g.tracker_enable     ?? 1,
    tracker_iou_thresh: g.tracker_iou_thresh ?? 0.3,
    tracker_max_miss:   g.tracker_max_miss   ?? 30,
    tracker_min_hits:   g.tracker_min_hits   ?? 3,
    performance_display: g.performance_display ?? 1,
    enable_pause_key:   g.enable_pause_key   ?? 0,
    enable_rtsp:        g.enable_rtsp        ?? 0,
  }
  // 透传 RTSP 高级字段(端口/路径/编码等)，让手改 config.json 的值在网页保存后不丢失
  for (const k of ['rtsp_port', 'rtsp_path', 'rtsp_fps', 'rtsp_bitrate', 'rtsp_codec', 'rtsp_encoder'] as const) {
    if (g[k] !== undefined && g[k] !== null) globalCfg[k] = g[k]
  }
  if (globalLogics.length > 0) globalCfg.global_logics = globalLogics

  // Auto tile layout
  if ((g.tile_rows ?? 0) === 0 || (g.tile_cols ?? 0) === 0) {
    const cols = Math.ceil(Math.sqrt(channels.length))
    globalCfg.tile_cols = cols
    globalCfg.tile_rows = Math.ceil(channels.length / cols)
  }

  return {
    config: { schema_version: 2, global: globalCfg, channels },
    roi:    roiOut,
  }
}
