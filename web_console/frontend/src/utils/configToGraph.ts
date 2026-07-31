import { Node, Edge, MarkerType } from '@xyflow/react'
import { sopParametersToFlow } from './sopFlow'
import type { GlobalLogicEntry } from '../components/GlobalLogicsPanel'
import type { GlobalSettingsData } from '../components/GlobalSettingsPanel'
import { DEFAULT_GLOBAL_SETTINGS } from '../components/GlobalSettingsPanel'
import type { Zone as RoiZone } from '../store/roiStore'
import { normalizeRoiPolygon } from './roiPolygon'

// NOTE: _ctr is declared inside configToGraph() below so node IDs are local to each call.
//       Canvas positions are restored from config._editor_layout (keyed by channel index +
//       node role), NOT by node ID — see `layout`/`pos` below. This survives ID changes,
//       drag/drop/paste, and channel add/remove.

// 模型节点字段；通道里除这些(及 stream/logic/上报运行字段)之外的键视为逻辑参数。
const MODEL_KEYS = new Set([
  'id', 'enable', 'infer_enable', 'threads', 'max_fps',
  'playback_fps', 'tracker_enable', 'tracker_iou_thresh', 'tracker_max_miss',
  'tracker_min_hits', 'roi_zones', 'models',
])


export function configToGraph(
  config: Record<string, unknown>,
): {
  nodes: Node[]
  edges: Edge[]
  roiMapping: Record<string, RoiZone[]>
  globalLogics: GlobalLogicEntry[]
  globalSettings: GlobalSettingsData
} {
  // Node IDs are local to this call (counter resets each call); they only wire up
  // nodes/edges within the produced graph. Canvas positions come from
  // config._editor_layout, keyed by channel index + role — not by these IDs.
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

  const global = config.global && typeof config.global === 'object' && !Array.isArray(config.global)
    ? config.global as Record<string, unknown> : {}
  const channels = (config.channels as Record<string, unknown>[]) ?? []

  // 画布坐标(由 graphToConfig 按「通道序号 + 角色」写入 _editor_layout)。
  // 下面每个节点优先取保存坐标，没有则退回默认排布。缺这个键(老配置/手写配置)→ 全用默认。
  const layout = (config._editor_layout as Record<string, Record<string, { x: number; y: number }>>) ?? {}

  // Global logics
  const rawGL = (global.global_logics as Record<string, unknown>[]) ?? []
  const globalLogics: GlobalLogicEntry[] = rawGL.map(gl => ({
    enable:           (gl.enable          as boolean) ?? true,
    logic:            (gl.logic           as string)  ?? 'global_default',
    channels:         (gl.channels        as number[]) ?? [],
    poll_interval_ms: (gl.poll_interval_ms as number) ?? 200,
    logic_parameters: gl.logic_parameters && typeof gl.logic_parameters === 'object' &&
      !Array.isArray(gl.logic_parameters)
      ? gl.logic_parameters as Record<string, unknown> : {},
  }))

  const { global_logics: _gl, ...rawGD } = global
  const globalSettings: GlobalSettingsData = { ...DEFAULT_GLOBAL_SETTINGS, ...rawGD }

  // Layout constants
  const STREAM_X  = 60
  const MODEL_X   = 380
  const ROI_X     = 60   // above stream: ROI belongs to the channel, not to a model
  const LOGIC_X   = 680
  const REPORT_X  = 940
  const ROW_H     = 360  // leave room for the ROI card above each stream

  let modelCoreIndex = 0
  channels.forEach((ch, idx) => {
    const y      = idx * ROW_H + 60
    const origId = (ch.id as number) ?? idx
    // 该通道保存的画布坐标(按角色)；pos() 取保存值，缺失则用传入的默认坐标
    const lay = layout[String(idx)] ?? {}
    const pos = (role: string, x: number, yy: number) => lay[role] ?? { x, y: yy }
    const stream = ch.stream as Record<string, unknown> ?? {}

    // 模型节点只来自 channels[].models[]；空数组表示传统/无推理通道。
    const configuredModels = Array.isArray(ch.models)
      ? (ch.models as Record<string, unknown>[]).filter(model =>
          model != null && typeof model === 'object')
      : []
    const modelConfigs = configuredModels
    const hasModel = modelConfigs.length > 0

    // ── Stream node — 每个通道独立创建，不做去重 ──
    // 通道号 (channel_id) 唯一。每个通道使用独立 StreamNode；同一 StreamNode 可连接多个模型。
    // 即使两个通道 URL 相同，也仍然分别创建各自的视频流节点。
    const streamId = uid('stream')
    nodes.push({
      id: streamId, type: 'stream',
      position: pos('stream', STREAM_X, y),
      data: { ...stream, channel_id: origId },
    })

    // ── 通道字段分流：模型字段 → 模型节点；逻辑参数只来自 logic_parameters ──
    const {
      stream: _s, logic: _lg, logic_parameters: _logicParameters,
      report_policy: _reportPolicy, report_parameters: _reportParameters,
      ...rest
    } = ch
    const modelData:   Record<string, unknown> = {}
    Object.entries(rest).forEach(([k, v]) => {
      if (MODEL_KEYS.has(k)) modelData[k] = v
    })
    delete modelData.models

    // ── Model node (仅 YOLO 通道) ──
    // 节点创建顺序: stream→model→roi→logic→report。画布坐标按「通道序号 + 角色」从
    // config._editor_layout 还原(见 pos())，与节点 ID 无关。
    const modelIds: string[] = []
    if (hasModel) {
      modelConfigs.forEach((configuredModel, modelIndex) => {
        const modelId = uid('model')
        const npuCore = configuredModel.npu_core ?? (modelCoreIndex % 3)
        modelCoreIndex++
        const role = modelIndex === 0 ? 'model' : `model_${modelIndex}`
        nodes.push({
          id: modelId, type: 'model',
          position: pos(role, MODEL_X, y + modelIndex * 90),
          data: {
            ...modelData,
            ...configuredModel,
            infer_enable: configuredModel.enable !== false,
            npu_core: npuCore,
          },
        })
        edges.push(edge(streamId, 'stream-out', modelId, 'stream-in', '#3b82f6'))
        modelIds.push(modelId)
      })
    }

    // ── ROI node (一个 ROI 节点 = 该通道的多个命名区域) ──
    const zones: RoiZone[] = Array.isArray(ch.roi_zones)
      ? (ch.roi_zones as RoiZone[])
          .map(z => ({
            name: z.name ?? '',
            polygon: normalizeRoiPolygon(Array.isArray(z.polygon) ? z.polygon : []),
          }))
          .filter(z => z.polygon.length >= 3)
      : []
    let roiId: string | null = null
    if (zones.length > 0) {
      roiId = uid('roi')
      nodes.push({
        id: roiId, type: 'roi',
        position: pos('roi', ROI_X, y - 220),
        data: {},
      })
      roiMapping[roiId] = zones
      // ROI 是通道级配置，始终直接连接视频流；与是否存在模型或逻辑无关。
      edges.push(edge(roiId, 'roi-out', streamId, 'roi-in', '#f97316'))
    }

    // ── Logic / SOP node（可选）── 配置未声明 logic 时不创建后处理节点。
    const logic   = String(_lg ?? '').trim()
    const isSop   = logic === 'logic_path_sop'
    let logicId: string | null = null
    // SOP: flow 来自 logic_parameters；普通逻辑也只读取同一参数对象。
    const moduleParameters = _logicParameters && typeof _logicParameters === 'object' && !Array.isArray(_logicParameters)
      ? _logicParameters as Record<string, unknown> : {}
    if (logic) {
      logicId = uid(isSop ? 'sop' : 'logic')
      const logicData: Record<string, unknown> = isSop
        ? { ...sopParametersToFlow(moduleParameters), logic_parameters: moduleParameters }
        : { logic, logic_parameters: moduleParameters }
      nodes.push({
        id: logicId, type: isSop ? 'sop' : 'logic',
        position: pos('logic', hasModel ? LOGIC_X : MODEL_X, y),  // 传统通道 logic 占据 model 的列位置, 更紧凑
        data: logicData,
      })
      if (hasModel) {
        modelIds.forEach(modelId =>
          edges.push(edge(modelId, 'logic-out', logicId!, 'logic-in', isSop ? '#06b6d4' : '#a855f7')))
      } else {
        // 传统/无推理通道: 视频流直连逻辑函数；ROI 已独立连接视频流。
        edges.push(edge(streamId, 'stream-out', logicId, 'logic-in', '#3b82f6'))
      }
    }

    // ── Report node ── 截图上报或事件视频任一启用时重建。
    const policyObj = (_reportPolicy && typeof _reportPolicy === 'object'
      ? _reportPolicy : null) as Record<string, unknown> | null
    const configuredDeliveries = Array.isArray(policyObj?.deliveries)
      ? policyObj!.deliveries as Record<string, unknown>[] : []
    const hasReportNode = logicId !== null && policyObj?.enabled === true
    if (hasReportNode && logicId) {
      const nodeDeliveries = configuredDeliveries.length > 0 ? configuredDeliveries : [{
        id: `delivery_ch${origId}`,
        enabled: true,
        profile_id: '',
        contract_id: '',
        media: [],
      }]
      nodeDeliveries.forEach((delivery, reportIndex) => {
        const reportId = uid('report')
        const reportData: Record<string, unknown> = {
          logic_name: logic,
          report_policy: {
            ...(policyObj ?? {}),
            enabled: true,
            deliveries: [{ ...delivery, enabled: delivery.enabled !== false }],
            image_overlay: policyObj?.image_overlay ?? 'custom',
            video_overlay: policyObj?.video_overlay ?? 'custom',
            video_pre_sec: policyObj?.video_pre_sec ?? 3,
            video_post_sec: policyObj?.video_post_sec ?? 3,
            video_fps: policyObj?.video_fps ?? 15,
          },
          report_parameters: (_reportParameters && typeof _reportParameters === 'object') ? _reportParameters : {},
        }
        const role = reportIndex === 0 ? 'report' : `report_${reportIndex}`
        const baseX = hasModel ? REPORT_X : LOGIC_X
        nodes.push({
          id: reportId, type: 'report',
          position: pos(role, baseX + reportIndex * 250, y),
          data: reportData,
        })
        edges.push(edge(logicId, 'report-out', reportId, 'report-in', '#ef4444'))
      })
    }
  })

  return { nodes, edges, roiMapping, globalLogics, globalSettings }
}
