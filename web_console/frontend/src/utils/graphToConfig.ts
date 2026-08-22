import { Node, Edge } from '@xyflow/react'
import { getSrcType } from './streamSource'
import { sopFlowToParameters, type SopFlow } from './sopFlow'
import type { GlobalLogicEntry } from './globalLogic'
import type { GlobalSettingsData } from '../components/GlobalSettingsPanel'
import type { Zone } from '../store/roiStore'
import { normalizeRoiPolygon } from './roiPolygon'

// 一个通道可有多个 ROI 区域: 区域名 + 多边形(归一化坐标)。(与 roiStore 的 Zone 同构)
export type RoiZone = Zone

function buildStream(d: Record<string, unknown>): Record<string, unknown> {
  // src_type 必填、不再自动推断；按【显式】类型分别落字段（新建节点已带 src_type）。
  const t = getSrcType(d)
  if (t === 'usb') {
    const s: Record<string, unknown> = { src_type: 'usb', device: d.device ?? '/dev/video81' }
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
  globalSettings: GlobalSettingsData,
): { config: Record<string, unknown> } | null {
  // 一个视频流节点就是一个通道锚点。后面可以连接零或多个模型，并可选连接一个
  // 后处理 logic；两者都不连接时仍是合法的纯视频显示通道。
  const anchors = nodes.filter(n => n.type === 'stream')
  if (anchors.length === 0) return null

  // config.channels 的运行顺序与 C++ 一致：按稳定的配置 channel_id 排序。
  // 画布位置只属于布局，拖动节点不能改变运行槽位、控制目标或录像归属。
  anchors.sort((a, b) => {
    const aid = Number((a.data as Record<string, unknown>).channel_id ?? 0)
    const bid = Number((b.data as Record<string, unknown>).channel_id ?? 0)
    return aid - bid || a.id.localeCompare(b.id)
  })

  const channels: Record<string, unknown>[] = []
  // 画布坐标按「通道序号 idx + 节点角色」记录(与 ROI 用同一套 idx 对齐)，随 config 一起存。
  // 重新加载时 configToGraph 据此还原各节点位置 —— 不依赖易变的节点 ID，拖动/粘贴/增删通道后依旧稳。
  // C++ 用 cJSON 按键名取值，忽略这个多余的键。
  const layout: Record<string, Record<string, { x: number; y: number }>> = {}
  const rp = (n: Node) => ({ x: Math.round(n.position.x), y: Math.round(n.position.y) })
  const channelIdByLogicNode = new Map<string, number>()

  anchors.forEach((streamNode, idx) => {
    const modelNodes = edges
      .filter(e => e.source === streamNode.id && e.sourceHandle === 'stream-out')
      .map(e => nodes.find(n => n.id === e.target))
      .filter((n): n is Node => n?.type === 'model')
      .sort((a, b) => a.position.y - b.position.y)
    const isModel = modelNodes.length > 0
    const modelDataList = modelNodes.map(n => n.data as Record<string, unknown>)
    const m = modelDataList[0] ?? {}
    const stream = buildStream(streamNode.data as Record<string, unknown>)

    const directLogicEdge = edges.find(e => e.source === streamNode.id && e.sourceHandle === 'stream-out' &&
      ['logic', 'sop'].includes(String(nodes.find(n => n.id === e.target)?.type ?? '')))
    const modelLogicEdge = modelNodes
      .map(modelNode => edges.find(e => e.source === modelNode.id && e.sourceHandle === 'logic-out'))
      .find((edge): edge is Edge => edge != null)
    const logicEdge = modelLogicEdge ?? directLogicEdge
    const logicNode = logicEdge ? nodes.find(n => n.id === logicEdge.target) ?? null : null

    const usedModelConfigIds = new Set<string>()
    const modelConfigs = modelNodes.map((node, modelIndex) => {
      const data = node.data as Record<string, unknown>
      const requestedId = String(data.id ?? '').trim()
      const baseId = requestedId || `model_${modelIndex}`
      let modelId = baseId
      let suffix = 2
      while (usedModelConfigIds.has(modelId)) modelId = `${baseId}_${suffix++}`
      usedModelConfigIds.add(modelId)
      return {
        id:             modelId,
        enable:         data.infer_enable !== false,
        model_type:     data.model_type ?? 'yolov8_det',
        model_path:     data.model_path ?? '',
        label_path:     data.label_path ?? '',
        version:        data.version ?? '',
        obj_thresh:     data.obj_thresh ?? 0.3,
        nms_thresh:     data.nms_thresh ?? 0.45,
        detect_classes: (data.detect_classes as string[]) ?? [],
        npu_core:       data.npu_core ?? -1,
      }
    })

    // ── Channel id: use stream's channel_id if set, else fall back to sorted position ──
    const chId = (streamNode.data as Record<string, unknown>).channel_id != null
      ? Number((streamNode.data as Record<string, unknown>).channel_id)
      : idx

    // ── ROI：直接连接视频流，表示通道级区域配置，与模型/逻辑拓扑解耦。 ──
    const roiEdge = edges.find(e =>
      e.target === streamNode.id && e.targetHandle === 'roi-in')
    const roiNode = roiEdge ? nodes.find(n => n.id === roiEdge.source) ?? null : null
    const zones: RoiZone[] = (roiEdge ? (roiZones[roiEdge.source] ?? []) : [])
      .map(z => ({
        name: z.name ?? '',
        polygon: normalizeRoiPolygon(Array.isArray(z.polygon) ? z.polygon : []),
      }))
      .filter(z => z.polygon.length >= 3)

    // ── Logic/SOP ── 后处理是可选步骤；多个模型连接时应汇入同一个逻辑节点。
    const isSop = logicNode?.type === 'sop'
    const l = logicNode ? (logicNode.data as Record<string, unknown>) : {}
    const logic = isSop ? 'logic_path_sop' : String(l.logic ?? '').trim()
    const hasLogic = logic.length > 0
    if (hasLogic && logicNode) channelIdByLogicNode.set(logicNode.id, chId)

    // ── Report ──
    const reportNodes = hasLogic && logicNode
      ? edges
          .filter(e => e.source === logicNode.id && e.sourceHandle === 'report-out')
          .map(e => nodes.find(n => n.id === e.target))
          .filter((n): n is Node => n?.type === 'report')
      : []
    const reportData = reportNodes.map(n => n.data as Record<string, unknown>)

    // ── 通道基础字段 ── YOLO 与传统通道字段集不同 ──
    const ch: Record<string, unknown> = isModel
      ? {
          id:             chId,
          enable:         true,                       // 通道存在即启用；YOLO 节点的开关现在控制 infer_enable
          infer_enable:   modelConfigs.some(model => model.enable !== false),
          stream,
          models:         modelConfigs,
        }
      : {
          // 传统/无推理通道: models 为空；C++ 跳过 NPU 推理，
          // 仍解码/显示；连接了 logic 时才以空 results 逐帧执行后处理。
          id:           chId,
          enable:       true,
          infer_enable: false,
          stream,
          models:       [],
        }

    // 不写 logic 字段即表示“不执行后处理模块”；模型检测结果仍由框架负责绘制。
    if (hasLogic) ch.logic = logic

    // 通用告警策略：每个通道保存独立投递列表和动态参数表。
    const reportPolicies = reportData.map((data, reportIndex) => {
      const policy = data.report_policy && typeof data.report_policy === 'object'
        ? data.report_policy as Record<string, unknown> : {}
      const configured = Array.isArray(policy.deliveries)
        ? policy.deliveries as Record<string, unknown>[] : []
      const delivery = configured[0] ?? {
        id: `delivery_${reportNodes[reportIndex].id}`,
        enabled: true,
        connection_id: '',
        contract_id: '',
        contract_revision: '',
        media: [],
      }
      return { policy, delivery, enabled: policy.enabled !== false && delivery.enabled !== false }
    })
    const reportEnabled = reportPolicies.some(item => item.enabled)
    const usedDeliveryIds = new Set<string>()
    const deliveries: Record<string, unknown>[] = reportPolicies.map((item, reportIndex) => {
      const configuredId = String(item.delivery.id ?? '')
      const id = configuredId && !usedDeliveryIds.has(configuredId)
        ? configuredId : `delivery_${reportNodes[reportIndex].id}`
      usedDeliveryIds.add(id)
      const normalized: Record<string, unknown> = {
        ...item.delivery,
        id,
        enabled: item.enabled,
      }
      delete normalized.mapping
      delete normalized.request
      delete normalized.success
      delete normalized.adapter
      return normalized
    })
    const reportPolicy: Record<string, unknown> = reportNodes.length > 0
      ? { ...(reportPolicies[0]?.policy ?? {}), enabled: reportEnabled, deliveries }
      : { enabled: false, deliveries: [] }

    // 视频录制参数取自视频上报节点；图片/视频叠加选项分别取对应媒体节点。
    const imagePolicy = reportPolicies.find(item =>
      item.enabled && Array.isArray(item.delivery.media)
      && (item.delivery.media.includes('annotated_image') || item.delivery.media.includes('raw_image'))
    )?.policy
    const videoPolicy = reportPolicies.find(item =>
      item.enabled && Array.isArray(item.delivery.media) && item.delivery.media.includes('video')
    )?.policy
    if (imagePolicy?.image_overlay != null) reportPolicy.image_overlay = imagePolicy.image_overlay
    if (videoPolicy) {
      for (const key of ['video_overlay', 'video_pre_sec', 'video_post_sec', 'video_fps', 'merge_window_sec']) {
        if (videoPolicy[key] != null) reportPolicy[key] = videoPolicy[key]
      }
    }

    // 多个独立上报节点的参数定义最终仍汇总到通道级配置，按参数 ID 去重。
    const parameterMap = new Map<string, Record<string, unknown>>()
    reportPolicies.forEach(({ policy }, reportIndex) => {
      const defs = Array.isArray(policy.parameters) ? policy.parameters as Record<string, unknown>[] : []
      defs.forEach((def, defIndex) => {
        const id = String(def.id ?? '')
        parameterMap.set(id || `__anonymous_${reportIndex}_${defIndex}`, def)
      })
    })
    reportPolicy.parameters = [...parameterMap.values()]
    ch.report_policy = reportPolicy
    ch.report_parameters = reportNodes.length > 0
      ? Object.assign({}, ...reportData.map(data =>
          data.report_parameters && typeof data.report_parameters === 'object' ? data.report_parameters : {}))
      : {}
    let moduleParameters = l.logic_parameters && typeof l.logic_parameters === 'object' && !Array.isArray(l.logic_parameters)
      ? l.logic_parameters as Record<string, unknown> : {}
    if (hasLogic && isSop) {
      moduleParameters = sopFlowToParameters(l as unknown as SopFlow)
    }
    if (hasLogic) ch.logic_parameters = moduleParameters

    // Per-channel tracker overrides (仅 YOLO 通道; 传统通道 m={} 自然跳过)
    if (m.tracker_enable   != null) ch.tracker_enable   = m.tracker_enable
    if (m.tracker_iou_thresh != null) ch.tracker_iou_thresh = m.tracker_iou_thresh
    if (m.tracker_max_miss != null) ch.tracker_max_miss = m.tracker_max_miss
    if (m.tracker_min_hits != null) ch.tracker_min_hits = m.tracker_min_hits
    if (m.threads          != null) ch.threads          = m.threads
    if (m.max_fps          != null) ch.max_fps          = m.max_fps

    // ROI 唯一持久化入口；空数组明确表示本通道没有 ROI。
    ch.roi_zones = zones

    // 记录该通道各角色节点的画布坐标(缺失的角色不写)，供重新加载时还原布局
    const slot: Record<string, { x: number; y: number }> = {}
    if (streamNode) slot.stream = rp(streamNode)
    modelNodes.forEach((modelNode, modelIndex) => {
      slot[modelIndex === 0 ? 'model' : `model_${modelIndex}`] = rp(modelNode)
    })
    if (roiNode)    slot.roi    = rp(roiNode)
    if (logicNode)  slot.logic  = rp(logicNode)
    reportNodes.forEach((reportNode, reportIndex) => {
      slot[reportIndex === 0 ? 'report' : `report_${reportIndex}`] = rp(reportNode)
    })
    layout[String(idx)] = slot

    channels.push(ch)
  })

  // ── Global logic ── 全局输入只来自画布上的 channel logic → global logic 连线。
  const globalLayout: Record<string, { x: number; y: number }> = {}
  const globalLogics: GlobalLogicEntry[] = nodes
    .filter(node => node.type === 'globalLogic')
    .sort((a, b) => a.position.y - b.position.y || a.position.x - b.position.x)
    .map((globalNode, globalIndex) => {
      const data = globalNode.data as Record<string, unknown>
      const channelsForGlobal = [...new Set(edges
        .filter(edge => edge.target === globalNode.id && edge.targetHandle === 'global-in')
        .map(edge => channelIdByLogicNode.get(edge.source))
        .filter((channelId): channelId is number => channelId != null))]
        .sort((a, b) => a - b)

      const reportNodes = edges
        .filter(edge => edge.source === globalNode.id && edge.sourceHandle === 'report-out')
        .map(edge => nodes.find(node => node.id === edge.target))
        .filter((node): node is Node => node?.type === 'report')
      const reportData = reportNodes.map(node => node.data as Record<string, unknown>)
      const reportPolicies = reportData.map((item, reportIndex) => {
        const policy = item.report_policy && typeof item.report_policy === 'object'
          ? item.report_policy as Record<string, unknown> : {}
        const configured = Array.isArray(policy.deliveries)
          ? policy.deliveries as Record<string, unknown>[] : []
        const delivery = configured[0] ?? {
          id: `delivery_${reportNodes[reportIndex].id}`,
          enabled: true,
          connection_id: '',
          contract_id: '',
          contract_revision: '',
          media: [],
        }
        return { policy, delivery, enabled: policy.enabled !== false && delivery.enabled !== false }
      })
      const reportEnabled = reportPolicies.some(item => item.enabled)
      const usedDeliveryIds = new Set<string>()
      const deliveries = reportPolicies.map((item, reportIndex) => {
        const configuredId = String(item.delivery.id ?? '')
        const id = configuredId && !usedDeliveryIds.has(configuredId)
          ? configuredId : `delivery_${reportNodes[reportIndex].id}`
        usedDeliveryIds.add(id)
        const normalized: Record<string, unknown> = {
          ...item.delivery,
          id,
          enabled: item.enabled,
        }
        delete normalized.mapping
        delete normalized.request
        delete normalized.success
        delete normalized.adapter
        return normalized
      })
      const reportPolicy: Record<string, unknown> = reportNodes.length > 0
        ? { ...(reportPolicies[0]?.policy ?? {}), enabled: reportEnabled, deliveries }
        : { enabled: false, deliveries: [] }
      const imagePolicy = reportPolicies.find(item =>
        item.enabled && Array.isArray(item.delivery.media)
        && (item.delivery.media.includes('annotated_image') || item.delivery.media.includes('raw_image')))?.policy
      const videoPolicy = reportPolicies.find(item =>
        item.enabled && Array.isArray(item.delivery.media)
        && item.delivery.media.includes('video'))?.policy
      if (imagePolicy?.image_overlay != null) reportPolicy.image_overlay = imagePolicy.image_overlay
      if (videoPolicy) {
        for (const key of ['video_overlay', 'video_pre_sec', 'video_post_sec', 'video_fps', 'merge_window_sec']) {
          if (videoPolicy[key] != null) reportPolicy[key] = videoPolicy[key]
        }
      }
      const parameterMap = new Map<string, Record<string, unknown>>()
      reportPolicies.forEach(({ policy }, reportIndex) => {
        const definitions = Array.isArray(policy.parameters)
          ? policy.parameters as Record<string, unknown>[] : []
        definitions.forEach((definition, definitionIndex) => {
          const id = String(definition.id ?? '')
          parameterMap.set(id || `__anonymous_${reportIndex}_${definitionIndex}`, definition)
        })
      })
      reportPolicy.parameters = [...parameterMap.values()]
      const applicationChannelIds = channels.map(channel => Number(channel.id))
      const requestedMediaChannel = Number(reportData[0]?.media_source_channel_id ?? -1)
      const mediaSourceChannel = applicationChannelIds.includes(requestedMediaChannel)
        ? requestedMediaChannel : undefined

      globalLayout[`logic_${globalIndex}`] = rp(globalNode)
      reportNodes.forEach((reportNode, reportIndex) => {
        globalLayout[`report_${globalIndex}_${reportIndex}`] = rp(reportNode)
      })

      return {
        instance_id: String(data.instance_id ?? ''),
        enable: data.enable !== false,
        logic: String(data.logic ?? ''),
        channels: channelsForGlobal,
        poll_interval_ms: Number(data.poll_interval_ms ?? 200),
        logic_parameters: data.logic_parameters && typeof data.logic_parameters === 'object'
          && !Array.isArray(data.logic_parameters)
          ? data.logic_parameters as Record<string, unknown> : {},
        report_policy: reportPolicy,
        report_parameters: reportNodes.length > 0
          ? Object.assign({}, ...reportData.map(item =>
              item.report_parameters && typeof item.report_parameters === 'object'
                ? item.report_parameters : {}))
          : {},
        media_source_channel_id: mediaSourceChannel,
      }
    })
  if (Object.keys(globalLayout).length > 0) layout.global = globalLayout

  // ── Global config ──
  const g = globalSettings
  const globalCfg: Record<string, unknown> = {
    enable_display:     g.enable_display     ?? 0,
    disp_width:         g.disp_width         ?? 640,
    disp_height:        g.disp_height        ?? 640,
    tile_rows:          g.tile_rows          ?? 1,
    tile_cols:          g.tile_cols          ?? 1,
    max_fps:            g.max_fps            ?? 25,
    queue_size:         g.queue_size         ?? 1,
    channel_threads:    g.channel_threads    ?? 3,
    tracker_enable:     g.tracker_enable     ?? 1,
    tracker_iou_thresh: g.tracker_iou_thresh ?? 0.3,
    tracker_max_miss:   g.tracker_max_miss   ?? 30,
    tracker_min_hits:   g.tracker_min_hits   ?? 3,
    performance_display: g.performance_display ?? 0,
    enable_pause_key:   g.enable_pause_key   ?? 0,
    enable_rtsp:        g.enable_rtsp        ?? 1,
    rtsp_codec:         'h264',
  }
  // 透传 RTSP 端口/路径/码率/编码器；Web 预览统一使用可零转码播放的 H264。
  for (const k of ['rtsp_port', 'rtsp_path', 'rtsp_bitrate', 'rtsp_encoder'] as const) {
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
    config: { global: globalCfg, channels, _editor_layout: layout },
  }
}
