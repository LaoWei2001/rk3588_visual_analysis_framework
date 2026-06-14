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
  NodeChange,
  Panel,
  MarkerType,
  ReactFlowInstance,
} from '@xyflow/react'
import '@xyflow/react/dist/style.css'

import StreamNode  from '../nodes/StreamNode'
import ModelNode   from '../nodes/ModelNode'
import ROINode     from '../nodes/ROINode'
import LogicNode   from '../nodes/LogicNode'
import ReportNode  from '../nodes/ReportNode'

import { useROIStore, type Zone } from '../store/roiStore'
import { useConsoleStore } from '../store/consoleStore'
import { useEditorStore }  from '../store/editorStore'
import { graphToConfig }   from '../utils/graphToConfig'
import { configToGraph }   from '../utils/configToGraph'
import {
  fetchConfig, fetchROI, saveConfig, saveROI, saveConfigFile, deleteConfigFile,
  fetchConfigFiles, loadConfigFile,
  fetchAppLogics, asLogicDef,
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

  const [nodes, setNodes, onNodesChange] = useNodesState<Node>([])
  const [edges, setEdges, onEdgesChange] = useEdgesState<Edge>([])
  const [rfInstance, setRfInstance]      = useState<ReactFlowInstance<Node, Edge> | null>(null)

  // Layout persistence: keep a ref so the debounced timer always sees latest nodes
  const nodesRef       = useRef<Node[]>([])
  const edgesRef       = useRef<Edge[]>([])
  const layoutTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null)
  // 复制/剪切的剪贴板（节点 + 内部连线 + ROI 区域）
  const clipboardRef   = useRef<{
    nodes: Node[]
    edges: Edge[]
    roi: Record<string, { zones: Zone[]; res?: [number, number] }>
  } | null>(null)
  useEffect(() => { nodesRef.current = nodes }, [nodes])
  useEffect(() => { edgesRef.current = edges }, [edges])

  const [saving,         setSaving]        = useState(false)
  const [toast,          setToast]         = useState<{ msg: string; ok: boolean } | null>(null)
  const [globalLogics,   setGlobalLogics]  = useState<GlobalLogicEntry[]>([])
  const [globalSettings, setGlobalSettings] = useState<GlobalSettingsData>(DEFAULT_GLOBAL_SETTINGS)
  const [reportByLogic,  setReportByLogic]  = useState<Record<string, string>>({})
  const [importFiles,    setImportFiles]   = useState<string[]>([])
  const [showImport,     setShowImport]    = useState(false)
  const [showServiceCfg, setShowServiceCfg] = useState(false)
  // 当前正在编辑/将保存到的配置文件（相对 app 目录）。导入/另存为后会切到对应文件，
  // 之后「保存」写到这里 —— 这样可以在副本上改而不动 config.json。
  const [currentFile,    setCurrentFile]   = useState('assets/config.json')

  const roiZones    = useROIStore(s => s.zones)
  const setAllROI   = useROIStore(s => s.setAll)
  const loadConsole = useConsoleStore(s => s.load)

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

  useEffect(() => {
    if (!appName) return
    // 打开「启动配置」选中的那份文件（缺省 config.json）；不存在则下面创建一个空的同名文件
    const base = (configParam || 'config.json').replace(/\\/g, '/').split('/').pop() || 'config.json'
    const targetRel = `assets/${base}`
    const isDefault = targetRel === 'assets/config.json'
    setCurrentFile(targetRel)
    ;(async () => {
      try {
        // logic→上报类型映射，与配置并行拉取
        const logicsP = fetchAppLogics(appName).catch(() => null)

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

        const logics = await logicsP
        const repMap: Record<string, string> = {}
        if (logics) logics.channel_logics.map(asLogicDef).forEach(d => { if (d.report) repMap[d.name] = d.report })
        setReportByLogic(repMap)
        applyConfig(cfg, roi, isDefault, repMap)
      } catch { /* blank canvas */ }
    })()
  }, [appName, configParam]) // eslint-disable-line

  // localStorage key for layout persistence (per app)
  const layoutKey = appName ? `flow_layout_${appName}` : null

  // applyConfig: load nodes/edges, restore saved positions, then fit view
  // restoreLayout=true: 初次加载本程序的 config.json 时，沿用用户拖拽过的坐标
  // restoreLayout=false: 导入"另一个"配置文件时，不沿用旧坐标(节点ID是确定性的,
  //                      沿用会让新配置贴到旧位置、看起来像没换)，用干净的默认排布
  const applyConfig = (
    cfg: Record<string, unknown>,
    roi: Record<string, RoiEntry>,
    restoreLayout = true,
    reportMap: Record<string, string> = reportByLogic,
  ) => {
    const { nodes: n, edges: e, roiMapping, globalLogics: gl, globalSettings: gs } =
      configToGraph(cfg, roi, reportMap)

    if (restoreLayout) {
      try {
        const saved = layoutKey ? localStorage.getItem(layoutKey) : null
        if (saved) {
          const pos: Record<string, { x: number; y: number }> = JSON.parse(saved)
          n.forEach(node => { if (pos[node.id]) node.position = { ...pos[node.id] } })
        }
      } catch { /* corrupted or unavailable — fall back to default layout */ }
    } else if (layoutKey) {
      try { localStorage.removeItem(layoutKey) } catch { /**/ }
    }

    setNodes(n)
    setEdges(e)
    setAllROI(roiMapping)
    setGlobalLogics(gl)
    setGlobalSettings(gs)
    // 加载/导入新配置 → 重置撤销历史（每份配置各自独立的历史）
    histRef.current = { stack: [], idx: -1, restoring: false }
    setTimeout(() => rfInstance?.fitView({ padding: 0.12, duration: 300 }), 50)
  }

  const showToast = (msg: string, ok = true) => {
    setToast({ msg, ok })
    setTimeout(() => setToast(null), 3000)
  }

  // ── Auto-save node positions after user drag (debounced 600 ms) ──
  const onNodesChangeWithLayout = useCallback((changes: NodeChange[]) => {
    onNodesChange(changes)
    if (!layoutKey || !changes.some(c => c.type === 'position')) return
    if (layoutTimerRef.current) clearTimeout(layoutTimerRef.current)
    layoutTimerRef.current = setTimeout(() => {
      const positions: Record<string, { x: number; y: number }> = {}
      nodesRef.current.forEach((n: Node) => { positions[n.id] = n.position })
      try { localStorage.setItem(layoutKey, JSON.stringify(positions)) } catch { /**/ }
    }, 600)
  }, [onNodesChange, layoutKey])

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
    const flash = (msg: string) => { setToast({ msg, ok: true }); window.setTimeout(() => setToast(null), 1500) }
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
      // 同样用干净布局, 不沿用旧坐标。
      applyConfig(cfg, {}, false)
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
      applyConfig({}, {}, false)         // 空白画布
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

    const result = graphToConfig(nodes, edges, roiZones, globalLogics, globalSettings, reportByLogic)
    if (!result) { showToast('配置生成失败', false); return null }
    return result as { config: Record<string, unknown>; roi: Record<string, RoiEntry> }
  }

  // ── 保存（写入当前文件 currentFile）──
  const handleSave = async () => {
    const result = buildConfig()
    if (!result || !appName) return
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
      showToast(`保存成功 ✓（${cfgBase(currentFile)}）`)
    } catch (e: unknown) {
      showToast(`保存失败: ${e instanceof Error ? e.message : String(e)}`, false)
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

  const nodeColor = (n: Node) => {
    if (n.type === 'stream') return '#2563eb'
    if (n.type === 'model')  return '#16a34a'
    if (n.type === 'roi')    return '#ea580c'
    if (n.type === 'logic')  return '#9333ea'
    return '#dc2626'
  }

  return (
    <div className="editor-page">
      {toast && <div className={`editor-toast ${toast.ok ? 'ok' : 'err'}`}>{toast.msg}</div>}

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

      {/* Toolbar */}
      <div className="editor-toolbar">
        <button className="tb-btn" onClick={() => navigate('/')}>← 返回</button>
        <span className="tb-title">
          {appName ? `配置: ${appName}` : '流程编辑器'}
          {appName && <span className="tb-file" title={`当前编辑的文件：${currentFile}`}>📄 {cfgBase(currentFile)}</span>}
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
            onNodesChange={onNodesChangeWithLayout}
            onEdgesChange={onEdgesChange}
            onConnect={onConnect}
            onInit={setRfInstance}
            nodeTypes={nodeTypes}
            deleteKeyCode="Delete"
            proOptions={{ hideAttribution: true }}
            selectionOnDrag={true}
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
