import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { useParams, useNavigate, useSearchParams } from 'react-router-dom'
import {
  ReactFlow,
  Background,
  Controls,
  MiniMap,
  addEdge,
  useNodesState,
  useEdgesState,
  Node, Edge, Connection,
  Panel,
  MarkerType,
  ReactFlowInstance,
  SelectionMode,
} from '@xyflow/react'
import '@xyflow/react/dist/style.css'

import StreamNode  from '../nodes/StreamNode'
import ModelNode   from '../nodes/ModelNode'
import ROINode     from '../nodes/ROINode'
import LogicNode   from '../nodes/LogicNode'
import SopNode     from '../nodes/SopNode'
import ReportNode  from '../nodes/ReportNode'

import { useROIStore, type Zone } from '../store/roiStore'
import { useConsoleStore } from '../store/consoleStore'
import { useEditorStore }  from '../store/editorStore'
import { useSopUiStore }   from '../store/sopUiStore'
import { graphToConfig }   from '../utils/graphToConfig'
import { configToGraph }   from '../utils/configToGraph'
import { saveLastConfig }  from '../utils/lastConfig'
import {
  fetchConfig, fetchROI, saveConfig, saveROI, saveConfigFile, deleteConfigFile,
  fetchConfigFiles, loadConfigFile,
  type RoiEntry,
} from '../api/client'
import GlobalLogicsPanel,  { GlobalLogicEntry }                    from '../components/GlobalLogicsPanel'
import GlobalSettingsPanel, { GlobalSettingsData, DEFAULT_GLOBAL_SETTINGS } from '../components/GlobalSettingsPanel'
import NodeConfigPanel from '../components/NodeConfigPanel'
import ServiceConfigModal from '../components/ServiceConfigModal'
import './EditorPage.css'

// ── Node types (defined outside component → stable reference) ──
const nodeTypes = {
  stream: StreamNode,
  model:  ModelNode,
  roi:    ROINode,
  logic:  LogicNode,
  sop:    SopNode,
  report: ReportNode,
}

// ── Edge color by handle ──
const EDGE_COLORS: Record<string, string> = {
  'stream-out': '#3b82f6',
  'roi-out':    '#f97316',
  'logic-out':  '#a855f7',
  'report-out': '#ef4444',
}

// ── Palette chip definitions ──
const PALETTE_NODES = [
  { type: 'stream', label: '视频流',   icon: '◈', cls: 'stream' },
  { type: 'model',  label: 'YOLO推理', icon: '🧠', cls: 'model'  },
  { type: 'roi',    label: 'ROI区域',  icon: '◆', cls: 'roi'    },
  { type: 'logic',  label: '逻辑函数', icon: '⚡', cls: 'logic'  },
  { type: 'sop',    label: 'SOP流程',  icon: '🧭', cls: 'sop'    },
  { type: 'report', label: '上报配置', icon: '📤', cls: 'report' },
] as const

// 切到 RTSP / 新建视频流节点时的默认地址
const DEFAULT_RTSP_URL = 'rtsp://admin:jndxc301@192.168.2.150/Streaming/Channels/101'

// ── Default node data when dropped ──
const NODE_DEFAULTS: Record<string, Record<string, unknown>> = {
  stream: { src_type: 'rtsp', url: DEFAULT_RTSP_URL, video_enc: 'h264', channel_id: 0 },
  model:  { enable: true, npu_core: 0, model_type: 'yolov8_det',
            model_path: '', label_path: '', obj_thresh: 0.3, nms_thresh: 0.45, detect_classes: [] },
  roi:    {},
  logic:  { logic: 'logic_default' },
  sop:    { target_label: '', reset_sec: 5, end_mode: 'leave', end_zone: '', steps: [] },
  report: { report_type: 'server', server_url: '' },
}

let _uid = 0
const uid = (p: string) => `${p}-${++_uid}-${Date.now()}`

// 'assets/config.json' → 'config.json'
const cfgBase = (p: string): string => p.split('/').pop() ?? p
// 由当前文件名推一个「另存为」默认名：config.json → config_copy.json
const suggestCopyName = (p: string): string => {
  const base = cfgBase(p)
  const stem = base.toLowerCase().endsWith('.json') ? base.slice(0, -5) : base
  return `${stem}_copy.json`
}

// ── 撤销/恢复 历史快照 ──
type HistorySnap = {
  nodes: Node[]
  edges: Edge[]
  roi: Record<string, Zone[]>
  gs: GlobalSettingsData
  gl: GlobalLogicEntry[]
}
// 计算签名(只取实质内容, 忽略 selected/dragging 等, 避免选中节点也记历史)
const histSig = (s: HistorySnap): string => JSON.stringify({
  n: s.nodes.map(n => ({ i: n.id, t: n.type, x: Math.round(n.position.x), y: Math.round(n.position.y), d: n.data })),
  e: s.edges.map(e => ({ i: e.id, s: e.source, t: e.target, sh: e.sourceHandle ?? null, th: e.targetHandle ?? null })),
  r: s.roi, g: s.gs, l: s.gl,
})

export default function EditorPage() {
  const { appName }    = useParams<{ appName: string }>()
  const [searchParams] = useSearchParams()
  const configParam    = searchParams.get('config')   // 由 AppsPage「启动配置」带入的目标配置文件名
  const navigate       = useNavigate()
  const setAppName     = useEditorStore(s => s.setAppName)
  const loadAssets     = useEditorStore(s => s.loadAssets)
  const setGlobalMaxFps = useEditorStore(s => s.setGlobalMaxFps)
  const dirty          = useEditorStore(s => s.dirty)       // 有未保存改动（也供侧边栏导航拦截）
  const setDirty       = useEditorStore(s => s.setDirty)

  const [nodes, setNodes, onNodesChange] = useNodesState<Node>([])
  const [edges, setEdges, onEdgesChange] = useEdgesState<Edge>([])
  const [rfInstance, setRfInstance]      = useState<ReactFlowInstance<Node, Edge> | null>(null)
  // 加载/导入配置后待执行的「自动 fit view」：等节点测量完 + 实例就绪再触发(见下方 effect)
  const pendingFitRef  = useRef(false)

  // Keep refs to the latest nodes/edges (used by paste / connect-validation / save)
  const nodesRef       = useRef<Node[]>([])
  const edgesRef       = useRef<Edge[]>([])
  // 复制/剪切的剪贴板（节点 + 内部连线 + ROI 区域）
  const clipboardRef   = useRef<{
    nodes: Node[]
    edges: Edge[]
    roi: Record<string, { zones: Zone[]; res?: [number, number] }>
  } | null>(null)
  useEffect(() => { nodesRef.current = nodes }, [nodes])
  useEffect(() => { edgesRef.current = edges }, [edges])

  // ── 未保存改动追踪 ──
  // savedSigRef: 上次「保存/加载」时的画布基线签名；当前签名与它不同 = 有未保存改动。
  // 初值 = 空画布签名 → 加载完成前(含异步拉取配置期间)空画布不会被误判为「有改动」。
  const savedSigRef        = useRef<string>(
    histSig({ nodes: [], edges: [], roi: {}, gs: DEFAULT_GLOBAL_SETTINGS, gl: [] })
  )
  const dirtyRef           = useRef(false)
  const toastTimer         = useRef<ReturnType<typeof setTimeout> | null>(null)

  const [saving,         setSaving]        = useState(false)
  const [toast,          setToast]         = useState<{ msg: string; ok: boolean } | null>(null)
  const [globalLogics,   setGlobalLogics]  = useState<GlobalLogicEntry[]>([])
  const [globalSettings, setGlobalSettings] = useState<GlobalSettingsData>(DEFAULT_GLOBAL_SETTINGS)
  const [importFiles,    setImportFiles]   = useState<string[]>([])
  const [showImport,     setShowImport]    = useState(false)
  const [showServiceCfg, setShowServiceCfg] = useState(false)
  const [leavePrompt,    setLeavePrompt]    = useState(false)   // 未保存退出时的「是否保存配置」弹窗
  // 当前正在编辑/将保存到的配置文件（相对 app 目录）。导入/另存为后会切到对应文件，
  // 之后「保存」写到这里 —— 这样可以在副本上改而不动 config.json。
  const [currentFile,    setCurrentFile]   = useState('assets/config.json')

  const roiZones    = useROIStore(s => s.zones)
  const setAllROI   = useROIStore(s => s.setAll)
  const loadConsole = useConsoleStore(s => s.load)
  // SOP 流程弹窗打开时, 暂停主画布的 Delete 删节点(避免误删整个 SOP 节点)
  const sopFlowOpen = useSopUiStore(s => s.flowOpen)

  // ── 撤销/恢复 历史栈 (Ctrl+Z / Ctrl+Y) ──
  const gsRef   = useRef(globalSettings)
  const glRef   = useRef(globalLogics)
  useEffect(() => { gsRef.current = globalSettings }, [globalSettings])
  useEffect(() => { glRef.current = globalLogics }, [globalLogics])
  const histRef = useRef<{ stack: HistorySnap[]; idx: number; restoring: boolean }>(
    { stack: [], idx: -1, restoring: false }
  )

  // ── Currently selected node → drives the config sidebar ──
  const selectedNode = useMemo(
    () => nodes.find(n => n.selected) ?? null,
    [nodes]
  )

  // ── Update node data from config panel (outside ReactFlow context) ──
  const handleUpdateNodeData = useCallback((nodeId: string, patch: Record<string, unknown>) => {
    setNodes(prev => prev.map(n =>
      n.id === nodeId
        ? { ...n, data: { ...(n.data as Record<string, unknown>), ...patch } }
        : n
    ))
  }, [setNodes])

  // Sync appName to editorStore, load assets
  useEffect(() => {
    if (appName) {
      setAppName(appName)
      loadAssets(appName)
    }
  }, [appName, setAppName, loadAssets])

  // 全局最大FPS → editorStore：ROINode 抓 USB 帧时按它推算采集分辨率(与 C++ 一致)，避免 ROI 错位
  useEffect(() => {
    setGlobalMaxFps(Number(globalSettings.max_fps ?? 15))
  }, [globalSettings.max_fps, setGlobalMaxFps])

  useEffect(() => { loadConsole() }, [loadConsole])

  // 有未保存改动时，拦截「关闭标签页 / 刷新 / 浏览器后退」——弹原生确认，防止误触丢失配置
  useEffect(() => {
    if (!dirty) return
    const onBeforeUnload = (e: BeforeUnloadEvent) => { e.preventDefault(); e.returnValue = '' }
    window.addEventListener('beforeunload', onBeforeUnload)
    return () => window.removeEventListener('beforeunload', onBeforeUnload)
  }, [dirty])

  // 离开编辑器（卸载）时清掉全局 dirty 标志，避免在别的页面残留导航拦截
  useEffect(() => () => { setDirty(false) }, [setDirty])

  useEffect(() => {
    if (!appName) return
    // 打开「启动配置」选中的那份文件（缺省 config.json）；不存在则下面创建一个空的同名文件
    const base = (configParam || 'config.json').replace(/\\/g, '/').split('/').pop() || 'config.json'
    const targetRel = `assets/${base}`
    const isDefault = targetRel === 'assets/config.json'
    setCurrentFile(targetRel)
    ;(async () => {
      try {
        let cfg: Record<string, unknown> | null = null
        let roi: Record<string, RoiEntry> = {}
        if (isDefault) {
          // 默认配置：沿用 roi_zones.json（编辑器 ROI 持久化），并恢复画布布局
          const [c, r] = await Promise.all([fetchConfig(appName), fetchROI(appName)])
          cfg = c as Record<string, unknown> | null
          roi = r ?? {}
        } else {
          // 其他配置：ROI 用文件自带的内嵌数据，不串用 roi_zones.json
          cfg = await loadConfigFile(appName, targetRel).catch(() => null)
        }

        // 文件不存在 → 落一个空配置，使其在磁盘上存在（之后保存写回这份文件）
        if (cfg == null) {
          try { await saveConfigFile(appName, targetRel, {}) }
          catch { /* 创建失败不阻断：仍以空白画布进入 */ }
          cfg = {}
        }

        applyConfig(cfg, roi)
      } catch { /* blank canvas */ }
    })()
  }, [appName, configParam]) // eslint-disable-line

  // applyConfig: load nodes/edges, then fit view.
  // 画布坐标随配置一起存(config._editor_layout, 见 graphToConfig/configToGraph)，由
  // configToGraph 按「通道序号 + 角色」还原 —— 每份配置自带各自的布局，互不影响，无需 localStorage。
  const applyConfig = (
    cfg: Record<string, unknown>,
    roi: Record<string, RoiEntry>,
  ) => {
    const { nodes: n, edges: e, roiMapping, globalLogics: gl, globalSettings: gs } =
      configToGraph(cfg, roi)

    setNodes(n)
    setEdges(e)
    setAllROI(roiMapping)
    setGlobalLogics(gl)
    setGlobalSettings(gs)
    // 加载/导入新配置 → 重置撤销历史（每份配置各自独立的历史）
    histRef.current = { stack: [], idx: -1, restoring: false }
    // 干净基线：直接用刚生成的图算签名，而不是等多次 setState 落定后再从实时状态采样。
    // 节点走 React state、ROI 走 Zustand store，二者可能分属不同 commit；若在中途采样基线，
    // 余下状态到位时就会被误判成「有改动」——这正是"什么都没动却提示未保存"的根因。
    savedSigRef.current = histSig({ nodes: n, edges: e, roi: roiMapping, gs, gl })
    if (dirtyRef.current) { dirtyRef.current = false; setDirty(false) }
    // 标记「这次加载完要自动 fit view」；具体何时触发交给下方 effect(等节点测量 + 实例就绪)
    pendingFitRef.current = true
  }

  // 自动 fit view：加载/导入配置后(pendingFitRef=true)，等节点进入状态且实例就绪再触发。
  // 用双 rAF 留一帧给 React Flow 测量节点尺寸，比固定 setTimeout(50) 可靠——每次打开/换配置都生效。
  // 节点是确定性进入状态的：实例后于节点就绪时此 effect 也会因 rfInstance 变化重跑，不会漏。
  useEffect(() => {
    if (!pendingFitRef.current || !rfInstance || nodes.length === 0) return
    let raf2 = 0
    const raf1 = requestAnimationFrame(() => {
      raf2 = requestAnimationFrame(() => {
        rfInstance.fitView({ padding: 0.12, duration: 300 })
        pendingFitRef.current = false
      })
    })
    return () => { cancelAnimationFrame(raf1); cancelAnimationFrame(raf2) }
  }, [nodes, rfInstance])

  const dismissToast = () => {
    if (toastTimer.current) { clearTimeout(toastTimer.current); toastTimer.current = null }
    setToast(null)
  }

  const showToast = (msg: string, ok = true) => {
    if (toastTimer.current) { clearTimeout(toastTimer.current); toastTimer.current = null }
    setToast({ msg, ok })
    // 成功提示 3s 自动消失；失败提示常驻，直到用户点 ✕ 关闭，避免错过「保存失败」等原因
    if (ok) toastTimer.current = setTimeout(() => { setToast(null); toastTimer.current = null }, 3000)
  }

  // 把当前画布标记为「已保存」（保存 / 另存成功后调用；此时画布未变，refs 即当前值）
  const markClean = () => {
    savedSigRef.current = histSig(makeHistorySnap())
    if (dirtyRef.current) { dirtyRef.current = false; setDirty(false) }
  }

  // ── Colored edges based on source handle ──
  const onConnect = useCallback((params: Connection) => {
    // 视频流节点 stream-out 只允许连一个下游节点（通道号唯一，不能一对多）
    if (params.sourceHandle === 'stream-out') {
      const alreadyUsed = edgesRef.current.some(
        e => e.source === params.source && e.sourceHandle === 'stream-out'
      )
      if (alreadyUsed) {
        showToast('视频流节点已连接 — 通道号唯一，一个视频流只能接一路（YOLO 推理或逻辑函数）', false)
        return
      }
    }
    const color = EDGE_COLORS[params.sourceHandle ?? ''] ?? '#4f8ef7'
    setEdges(eds => addEdge({
      ...params,
      type: 'default',
      style: { stroke: color, strokeWidth: 1.5 },
      markerEnd: { type: MarkerType.ArrowClosed, color },
    }, eds))
  }, [setEdges])

  // ── Drag-from-palette ──
  const onDragOver = useCallback((event: React.DragEvent) => {
    event.preventDefault()
    event.dataTransfer.dropEffect = 'move'
  }, [])

  const onDrop = useCallback((event: React.DragEvent) => {
    event.preventDefault()
    const nodeType = event.dataTransfer.getData('application/reactflow')
    if (!nodeType || !rfInstance) return
    const position = rfInstance.screenToFlowPosition({ x: event.clientX, y: event.clientY })
    const id = uid(nodeType)
    setNodes(ns => [...ns, {
      id,
      type: nodeType,
      position,
      data: { ...(NODE_DEFAULTS[nodeType] ?? {}) },
    } as Node])
  }, [rfInstance, setNodes])

  // ── 复制 / 剪切 / 粘贴 选中节点 (Ctrl+C / X / V) ──
  const copySelection = useCallback((): number => {
    const sel = nodesRef.current.filter(n => n.selected)
    if (sel.length === 0) return 0
    const selIds = new Set(sel.map(n => n.id))
    // 只带上两端都在选区里的连线
    const innerEdges = edgesRef.current.filter(e => selIds.has(e.source) && selIds.has(e.target))
    const { zones, resolutions } = useROIStore.getState()
    const roi: Record<string, { zones: Zone[]; res?: [number, number] }> = {}
    sel.forEach(n => {
      if (n.type === 'roi' && zones[n.id]?.length) roi[n.id] = { zones: zones[n.id], res: resolutions[n.id] }
    })
    clipboardRef.current = {
      nodes: sel.map(n => ({ ...n, data: { ...(n.data as Record<string, unknown>) } })),
      edges: innerEdges.map(e => ({ ...e })),
      roi,
    }
    return sel.length
  }, [])

  const deleteSelection = useCallback((): number => {
    const selIds = new Set(nodesRef.current.filter(n => n.selected).map(n => n.id))
    if (selIds.size === 0) return 0
    const { clearZones } = useROIStore.getState()
    nodesRef.current.forEach(n => { if (selIds.has(n.id) && n.type === 'roi') clearZones(n.id) })
    setNodes(ns => ns.filter(n => !selIds.has(n.id)))
    setEdges(es => es.filter(e => !selIds.has(e.source) && !selIds.has(e.target)))
    return selIds.size
  }, [setNodes, setEdges])

  const pasteClipboard = useCallback((): number => {
    const clip = clipboardRef.current
    if (!clip || clip.nodes.length === 0) return 0
    const OFFSET = 40
    const idMap = new Map<string, string>()

    // 给粘贴出来的视频流节点分配未占用的通道号, 避免通道号撞车
    const usedCh = new Set<number>(
      nodesRef.current.filter(n => n.type === 'stream')
        .map(n => Number((n.data as Record<string, unknown>).channel_id ?? 0))
    )
    let probe = 0
    const nextFreeCh = () => { while (usedCh.has(probe)) probe++; usedCh.add(probe); return probe }

    const newNodes: Node[] = clip.nodes.map(n => {
      const newId = uid(n.type ?? 'node')
      idMap.set(n.id, newId)
      const data = { ...(n.data as Record<string, unknown>) }
      if (n.type === 'stream') data.channel_id = nextFreeCh()
      return { ...n, id: newId, selected: true,
               position: { x: n.position.x + OFFSET, y: n.position.y + OFFSET }, data } as Node
    })
    const newEdges: Edge[] = clip.edges.map(e => ({
      ...e, id: uid('e'),
      source: idMap.get(e.source) as string,
      target: idMap.get(e.target) as string,
    }))

    // ROI 区域复制到新节点 ID
    const { setZones } = useROIStore.getState()
    Object.entries(clip.roi).forEach(([oldId, v]) => {
      const newId = idMap.get(oldId)
      if (newId) setZones(newId, v.zones, v.res?.[0], v.res?.[1])
    })

    setNodes(ns => ns.map((n): Node => ({ ...n, selected: false })).concat(newNodes))
    setEdges(es => es.concat(newEdges))

    // 连续粘贴逐次错开, 不重叠
    clipboardRef.current = {
      ...clip,
      nodes: clip.nodes.map(n => ({ ...n, position: { x: n.position.x + OFFSET, y: n.position.y + OFFSET } })),
    }
    return newNodes.length
  }, [setNodes, setEdges])

  // ── 撤销 / 恢复 ──
  const makeHistorySnap = useCallback((): HistorySnap => ({
    nodes: nodesRef.current.map(n => ({ ...n, data: { ...(n.data as Record<string, unknown>) } })),
    edges: edgesRef.current.map(e => ({ ...e })),
    roi: JSON.parse(JSON.stringify(useROIStore.getState().zones)),
    gs: { ...gsRef.current },
    gl: glRef.current.map(x => ({ ...x })),
  }), [])

  const restoreHistory = useCallback((s: HistorySnap) => {
    histRef.current.restoring = true
    setNodes(s.nodes.map(n => ({ ...n, data: { ...(n.data as Record<string, unknown>) } })))
    setEdges(s.edges.map(e => ({ ...e })))
    useROIStore.getState().setAll(JSON.parse(JSON.stringify(s.roi)))
    setGlobalSettings({ ...s.gs })
    setGlobalLogics(s.gl.map(x => ({ ...x })))
  }, [setNodes, setEdges])

  const undo = useCallback((): boolean => {
    const h = histRef.current
    if (h.idx <= 0) return false
    h.idx -= 1
    restoreHistory(h.stack[h.idx])
    return true
  }, [restoreHistory])

  const redo = useCallback((): boolean => {
    const h = histRef.current
    if (h.idx >= h.stack.length - 1) return false
    h.idx += 1
    restoreHistory(h.stack[h.idx])
    return true
  }, [restoreHistory])

  // 防抖记录历史：状态稳定 400ms 后入栈（自然合并连续拖拽/输入）；跳过 restore 自身引发的变更
  useEffect(() => {
    const h = histRef.current
    // ── 未保存改动判定：当前画布签名与「上次保存/加载」的基线比对（含撤销/恢复后）──
    // 基线在 applyConfig / markClean 里同步设定，这里只做纯比较，不再从实时状态采样基线。
    const curSig = histSig(makeHistorySnap())
    const nd = curSig !== savedSigRef.current
    if (nd !== dirtyRef.current) { dirtyRef.current = nd; setDirty(nd) }
    if (h.restoring) { h.restoring = false; return }
    const timer = setTimeout(() => {
      const snap = makeHistorySnap()
      if (h.idx >= 0 && histSig(h.stack[h.idx]) === histSig(snap)) return
      h.stack = h.stack.slice(0, h.idx + 1)
      h.stack.push(snap)
      if (h.stack.length > 60) h.stack.shift()
      h.idx = h.stack.length - 1
    }, 400)
    return () => clearTimeout(timer)
  }, [nodes, edges, roiZones, globalSettings, globalLogics, makeHistorySnap])

  // ── 数字输入框：禁用方向键增减 + 滚轮改值，只能键盘直接输入 ──
  useEffect(() => {
    const isNumInput = (el: EventTarget | null): el is HTMLInputElement =>
      el instanceof HTMLInputElement && el.type === 'number'
    const onKeyDown = (e: KeyboardEvent) => {
      if ((e.key === 'ArrowUp' || e.key === 'ArrowDown') && isNumInput(e.target)) e.preventDefault()
    }
    const onWheel = (e: WheelEvent) => {
      if (isNumInput(e.target) && e.target === document.activeElement) e.preventDefault()
    }
    document.addEventListener('keydown', onKeyDown, true)
    document.addEventListener('wheel', onWheel, { passive: false })
    return () => {
      document.removeEventListener('keydown', onKeyDown, true)
      document.removeEventListener('wheel', onWheel)
    }
  }, [])

  // ── Ctrl + C / X / V / Z / Y 快捷键 ──
  useEffect(() => {
    const flash = (msg: string) => {
      if (toastTimer.current) clearTimeout(toastTimer.current)
      setToast({ msg, ok: true })
      toastTimer.current = setTimeout(() => { setToast(null); toastTimer.current = null }, 1500)
    }
    const onKey = (e: KeyboardEvent) => {
      if (!(e.ctrlKey || e.metaKey) || e.altKey) return
      // 在输入框/下拉里打字时不抢快捷键
      const t = e.target as HTMLElement | null
      if (t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' ||
                t.tagName === 'SELECT' || t.isContentEditable)) return
      const k = e.key.toLowerCase()
      // 撤销 / 恢复（Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z）
      if (k === 'z' && !e.shiftKey) { if (undo()) { e.preventDefault(); flash('已撤销') } return }
      if (k === 'y' || (k === 'z' && e.shiftKey)) { if (redo()) { e.preventDefault(); flash('已恢复') } return }
      // 复制 / 剪切 / 粘贴（不带 Shift）
      if (e.shiftKey) return
      if (k === 'c') { const n = copySelection(); if (n) { e.preventDefault(); flash(`已复制 ${n} 个节点`) } }
      else if (k === 'x') { const n = copySelection(); if (n) { deleteSelection(); e.preventDefault(); flash(`已剪切 ${n} 个节点`) } }
      else if (k === 'v') { const n = pasteClipboard(); if (n) { e.preventDefault(); flash(`已粘贴 ${n} 个节点`) } }
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [copySelection, deleteSelection, pasteClipboard, undo, redo])

  // ── Import ──
  const handleImportClick = async () => {
    if (!appName) return
    try {
      const files = await fetchConfigFiles(appName)
      setImportFiles(files); setShowImport(true)
    } catch { showToast('获取文件列表失败', false) }
  }

  const handleImportFile = async (filePath: string) => {
    if (!appName) return
    setShowImport(false)
    try {
      const cfg = await loadConfigFile(appName, filePath)
      // 导入另一份配置: ROI 只用该配置自带的(ch.roi_polygon)，
      // 不去拉本 App 当前的 roi_zones.json(那是上一份配置的 ROI，会串用)。
      // 画布布局也用该文件自带的 _editor_layout（每份配置自带布局，互不串用）。
      applyConfig(cfg, {})
      setCurrentFile(filePath)   // 之后「保存」写回这份文件，不动 config.json
      showToast(`已加载 ${filePath}（保存将写入此文件）`)
    } catch (e: unknown) {
      showToast(`加载失败: ${e instanceof Error ? e.message : String(e)}`, false)
    }
  }

  // ── 新建：创建一个空配置文件并切到空白画布编辑它 ──
  const handleNewFile = async () => {
    if (!appName) { showToast('未选择程序', false); return }
    const input = window.prompt('新建配置文件名（保存到 assets/ 下，省略 .json 会自动补全）：', 'config_new.json')
    if (input == null) return
    let fname = input.trim().replace(/\\/g, '/').split('/').pop() ?? ''
    if (!fname) { showToast('文件名不能为空', false); return }
    if (!fname.toLowerCase().endsWith('.json')) fname += '.json'
    if (fname === 'roi_zones.json') { showToast('该文件名被占用，请换一个', false); return }
    const path = `assets/${fname}`
    if (importFiles.includes(path) || path === 'assets/config.json') {
      if (!window.confirm(`${fname} 已存在，覆盖为一个空配置？`)) return
    }
    setSaving(true)
    try {
      const r = await saveConfigFile(appName, path, {})
      applyConfig({}, {})                // 空白画布
      setCurrentFile(r.path)             // 之后「保存」写到新文件
      setShowImport(false)
      showToast(`已新建 ${fname}，现在编辑的是空配置`)
    } catch (e: unknown) {
      showToast(`新建失败: ${e instanceof Error ? e.message : String(e)}`, false)
    } finally { setSaving(false) }
  }

  // ── 删除一个配置文件 ──
  const handleDeleteFile = async (filePath: string) => {
    if (!appName) return
    const base = cfgBase(filePath)
    const isDefault = filePath === 'assets/config.json'
    const msg = isDefault
      ? `确定删除默认配置 ${base}？\n删除后该程序将没有默认配置，需重新新建或保存后才能以默认配置启动。`
      : `确定删除配置文件 ${base}？此操作不可恢复。`
    if (!window.confirm(msg)) return
    try {
      await deleteConfigFile(appName, filePath)
      let files: string[] = []
      try { files = await fetchConfigFiles(appName) } catch { /* 列表刷新失败不致命 */ }
      setImportFiles(files)
      if (files.length === 0) setShowImport(false)
      // 删的是当前正在编辑的文件 → 回到默认 config.json（保存会重新创建它）
      if (filePath === currentFile) {
        setCurrentFile('assets/config.json')
        showToast(`已删除 ${base}（当前编辑切回 config.json）`)
      } else {
        showToast(`已删除 ${base}`)
      }
    } catch (e: unknown) {
      showToast(`删除失败: ${e instanceof Error ? e.message : String(e)}`, false)
    }
  }

  // ── 由当前画布生成配置（含校验）；失败时弹 toast 并返回 null ──
  const buildConfig = (): { config: Record<string, unknown>; roi: Record<string, RoiEntry> } | null => {
    if (!appName) { showToast('未选择程序', false); return null }
    // 通道锚点 = YOLO 推理节点，或被「视频流」直连的逻辑函数节点（传统 / 无推理通道）。
    const hasModel = nodes.some(n => n.type === 'model')
    const hasDirectLogic = nodes.some(n =>
      n.type === 'logic' &&
      edges.some(e => e.target === n.id && e.targetHandle === 'logic-in'
                      && nodes.find(s => s.id === e.source)?.type === 'stream'))
    if (!hasModel && !hasDirectLogic) {
      showToast('画布为空：请添加 YOLO 推理节点，或把视频流直接连到逻辑函数节点', false)
      return null
    }

    // ── 检测重复通道号（检查所有真正接入下游的视频流节点：接 YOLO 推理 或 直连逻辑函数）──
    const connectedStreamIds = new Set(
      edges
        .filter(e => e.targetHandle === 'stream-in' || e.targetHandle === 'logic-in')
        .map(e => e.source)
    )
    const dupSet = new Set<number>()
    const dupNums: number[] = []
    nodes
      .filter(n => n.type === 'stream' && connectedStreamIds.has(n.id))
      .forEach(n => {
        const cid = Number((n.data as Record<string, unknown>).channel_id ?? 0)
        if (dupSet.has(cid)) dupNums.push(cid)
        dupSet.add(cid)
      })
    if (dupNums.length > 0) {
      showToast(`通道号重复：Ch.${[...new Set(dupNums)].join('、')} — 请在视频流节点中修改后再保存`, false)
      return null
    }

    const result = graphToConfig(nodes, edges, roiZones, globalLogics, globalSettings)
    if (!result) { showToast('配置生成失败', false); return null }
    return result as { config: Record<string, unknown>; roi: Record<string, RoiEntry> }
  }

  // ── 保存（写入当前文件 currentFile）──
  const handleSave = async (): Promise<boolean> => {
    const result = buildConfig()
    if (!result || !appName) return false
    setSaving(true)
    try {
      if (currentFile === 'assets/config.json') {
        // 默认配置：保持原行为，同时落 roi_zones.json（编辑器 ROI 持久化）
        await saveConfig(appName, result.config)
        await saveROI(appName, result.roi)
      } else {
        // 副本/非默认配置：只写该文件（ROI 已内嵌进 config，自带自足，不动 config.json）
        await saveConfigFile(appName, currentFile, result.config)
      }
      markClean()
      saveLastConfig(appName, cfgBase(currentFile))   // 让「程序管理」返回后默认选中刚保存的这份配置
      showToast(`保存成功 ✓（${cfgBase(currentFile)}）`)
      return true
    } catch (e: unknown) {
      showToast(`保存失败: ${e instanceof Error ? e.message : String(e)}`, false)
      return false
    } finally { setSaving(false) }
  }

  // ── 另存为一份新配置文件，并切到该文件继续编辑（不影响原文件）──
  const handleSaveAs = async () => {
    const result = buildConfig()
    if (!result || !appName) return
    const input = window.prompt('另存为新配置文件名（保存到 assets/ 下，省略 .json 会自动补全）：', suggestCopyName(currentFile))
    if (input == null) return
    let fname = input.trim().replace(/\\/g, '/').split('/').pop() ?? ''
    if (!fname) { showToast('文件名不能为空', false); return }
    if (!fname.toLowerCase().endsWith('.json')) fname += '.json'
    if (fname === 'roi_zones.json') { showToast('该文件名被占用，请换一个', false); return }
    const path = `assets/${fname}`
    if (importFiles.includes(path) || path === 'assets/config.json') {
      if (!window.confirm(`${fname} 已存在，覆盖它？`)) return
    }
    setSaving(true)
    try {
      const r = await saveConfigFile(appName, path, result.config)
      setCurrentFile(r.path)                       // 切到新文件：后续「保存」写到它
      markClean()                                  // 副本内容 = 当前画布 → 标记为已保存
      saveLastConfig(appName, cfgBase(r.path))     // 让「程序管理」返回后默认选中这份副本
      showToast(`已另存为 ${fname}，现在编辑的是这份副本`)
      try { setImportFiles(await fetchConfigFiles(appName)) } catch { /* 列表刷新失败不致命 */ }
    } catch (e: unknown) {
      showToast(`另存失败: ${e instanceof Error ? e.message : String(e)}`, false)
    } finally { setSaving(false) }
  }

  // ── 导出当前配置到本地（浏览器下载）──
  const handleExport = () => {
    const result = buildConfig()
    if (!result) return
    const fileName = cfgBase(currentFile)
    const blob = new Blob([JSON.stringify(result.config, null, 2)], { type: 'application/json' })
    const url  = URL.createObjectURL(blob)
    const a    = document.createElement('a')
    a.href = url
    a.download = fileName
    document.body.appendChild(a)
    a.click()
    a.remove()
    URL.revokeObjectURL(url)
    showToast(`已导出 ${fileName}`)
  }

  // 返回程序管理：有未保存改动 → 弹「是否保存配置」(保存并离开 / 不保存离开 / 取消)
  const handleBack = () => {
    if (dirty) { setLeavePrompt(true); return }
    navigate('/')
  }

  const nodeColor = (n: Node) => {
    if (n.type === 'stream') return '#2563eb'
    if (n.type === 'model')  return '#16a34a'
    if (n.type === 'roi')    return '#ea580c'
    if (n.type === 'logic')  return '#9333ea'
    if (n.type === 'sop')    return '#06b6d4'
    return '#dc2626'
  }

  return (
    <div className="editor-page">
      {toast && (
        <div className={`editor-toast ${toast.ok ? 'ok' : 'err'}`}>
          <span>{toast.msg}</span>
          {!toast.ok && (
            <button className="editor-toast-close" onClick={dismissToast} title="关闭">✕</button>
          )}
        </div>
      )}

      {showServiceCfg && appName && (
        <ServiceConfigModal appName={appName} onClose={() => setShowServiceCfg(false)} onToast={showToast} />
      )}

      {/* Config files dialog: open / delete */}
      {showImport && (
        <div className="import-overlay" onClick={() => setShowImport(false)}>
          <div className="import-dialog" onClick={e => e.stopPropagation()}>
            <div className="import-header">
              配置文件（点击打开，🗑 删除）
              <button onClick={() => setShowImport(false)}>✕</button>
            </div>
            {importFiles.length === 0
              ? <div className="import-empty">未找到 JSON 文件</div>
              : importFiles.map(f => (
                  <div key={f} className="import-file-row">
                    <button className="import-file-btn" onClick={() => handleImportFile(f)}>{cfgBase(f)}</button>
                    <button className="import-del-btn" title="删除该配置文件"
                            onClick={() => handleDeleteFile(f)}>🗑</button>
                  </div>
                ))
            }
            <button className="import-new-btn" onClick={handleNewFile}>➕ 新建配置文件</button>
          </div>
        </div>
      )}

      {/* 未保存退出确认: 保存并离开 / 不保存离开 / 取消 */}
      {leavePrompt && (
        <div className="import-overlay" onClick={() => setLeavePrompt(false)}>
          <div className="import-dialog" onClick={e => e.stopPropagation()} style={{ maxWidth: 440 }}>
            <div className="import-header">
              有未保存的改动 — 是否保存配置？
              <button onClick={() => setLeavePrompt(false)}>✕</button>
            </div>
            <div style={{ padding: '16px 18px', fontSize: 13, lineHeight: 1.6 }}>
              画布有未保存的修改。离开前是否保存到 <b>{cfgBase(currentFile)}</b>？
            </div>
            <div style={{ display: 'flex', gap: 8, justifyContent: 'flex-end', padding: '0 16px 16px' }}>
              <button className="tb-btn" onClick={() => setLeavePrompt(false)}>取消</button>
              <button className="tb-btn" onClick={() => { setLeavePrompt(false); navigate('/') }}>不保存离开</button>
              <button className="tb-btn save" disabled={saving}
                onClick={async () => { setLeavePrompt(false); if (await handleSave()) navigate('/') }}>
                {saving ? '保存中…' : '保存并离开'}
              </button>
            </div>
          </div>
        </div>
      )}

      {/* Toolbar */}
      <div className="editor-toolbar">
        <button className="tb-btn" onClick={handleBack}>← 返回</button>
        <span className="tb-title">
          {appName ? `配置: ${appName}` : '流程编辑器'}
          {appName && <span className="tb-file" title={`当前编辑的文件：${currentFile}`}>📄 {cfgBase(currentFile)}</span>}
          {dirty && <span className="tb-dirty" title="有未保存的改动">● 未保存</span>}
        </span>
        <div className="tb-actions">
          <button className="tb-btn" onClick={() => setShowServiceCfg(true)}>⚙ 服务配置</button>
          <button className="tb-btn" onClick={handleNewFile} disabled={saving}>➕ 新建</button>
          <button className="tb-btn import" onClick={handleImportClick}>📂 配置文件</button>
          <button className="tb-btn" onClick={handleExport} disabled={saving}>⬇ 导出</button>
          <button className="tb-btn" onClick={handleSaveAs} disabled={saving}>🗐 另存为</button>
          <button className="tb-btn save"   onClick={handleSave} disabled={saving}>
            {saving ? '保存中…' : '💾 保存'}
          </button>
        </div>
      </div>

      {/* Draggable node palette */}
      <div className="node-palette">
        <span className="palette-label">拖拽到画布：</span>
        {PALETTE_NODES.map(p => (
          <div
            key={p.type}
            className={`palette-chip ${p.cls}`}
            draggable
            onDragStart={e => {
              e.dataTransfer.setData('application/reactflow', p.type)
              e.dataTransfer.effectAllowed = 'move'
            }}
          >
            {p.icon} {p.label}
          </div>
        ))}
      </div>

      {/* Canvas + config sidebar */}
      <div className="editor-main">
        <div className="flow-container">
          <ReactFlow
            nodes={nodes}
            edges={edges}
            onNodesChange={onNodesChange}
            onEdgesChange={onEdgesChange}
            onConnect={onConnect}
            onInit={setRfInstance}
            nodeTypes={nodeTypes}
            deleteKeyCode={sopFlowOpen ? null : 'Delete'}
            proOptions={{ hideAttribution: true }}
            selectionOnDrag={true}
            selectionMode={SelectionMode.Partial}
            panOnDrag={[1, 2]}
            onDrop={onDrop}
            onDragOver={onDragOver}
            minZoom={0.05}
          >
            <Background color="#2e3352" gap={20} size={1} />
            <Controls />
            <MiniMap nodeColor={nodeColor} maskColor="rgba(15,17,23,0.8)" />
            <Panel position="bottom-left">
              <div className="flow-hint">
                左键拖拽空白处 = 框选节点 · 中键平移视角 · 单击节点查看配置 · Delete 删除 · Ctrl+C/X/V 复制/剪切/粘贴
              </div>
            </Panel>
            {nodes.length === 0 && (
              <Panel position="top-center">
                <div className="canvas-empty-hint">
                  从上方拖拽节点到画布，按顺序连线：
                  <strong style={{color:'#3b82f6'}}>视频流</strong> →{' '}
                  <strong style={{color:'#16a34a'}}>YOLO推理</strong> →{' '}
                  <strong style={{color:'#9333ea'}}>逻辑函数</strong> →{' '}
                  <strong style={{color:'#dc2626'}}>上报配置</strong>
                  （ROI 区域可选，连到节点顶部）
                  <br />
                  <span style={{ opacity: 0.85 }}>
                    不用 YOLO？把 <strong style={{color:'#3b82f6'}}>视频流</strong> 直接连到{' '}
                    <strong style={{color:'#9333ea'}}>逻辑函数</strong>，即为传统 CV / 无推理通道
                  </span>
                </div>
              </Panel>
            )}
          </ReactFlow>
        </div>

        {/* Right config sidebar */}
        <NodeConfigPanel node={selectedNode} onUpdate={handleUpdateNodeData} />
      </div>

      {/* Bottom panels */}
      <GlobalSettingsPanel settings={globalSettings} onChange={setGlobalSettings} />
      <GlobalLogicsPanel   logics={globalLogics}     onChange={setGlobalLogics}   />
    </div>
  )
}
