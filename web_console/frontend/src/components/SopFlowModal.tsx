import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import {
  ReactFlow, ReactFlowProvider, Background, Controls, Panel,
  useNodesState, useEdgesState, addEdge, useReactFlow, SelectionMode,
  Node, Edge, Connection, MarkerType,
} from '@xyflow/react'
import '@xyflow/react/dist/style.css'
import SopStepNode from '../nodes/SopStepNode'
import SopEndStepNode from '../nodes/SopEndStepNode'
import SopSelfLoopEdge from './SopSelfLoopEdge'
import { type SopFlow, type SopStep, type SopEndMode, type SopTriggerMode, DEFAULT_STEP_ENTER, DEFAULT_STEP_DWELL, DEFAULT_STEP_DWELL_MAX } from '../utils/sopFlow'
import './SopFlowModal.css'

// 「SOP 流程配置」弹窗: 点 SOP 节点上的「配置流程」进来。
// 自包含、与外部解耦: 只接收 (上游 ROI 节点的)可选区域名 + 初始流程, 编辑完回吐一个 SopFlow。
//
// 子画布架构 (与主画布一致): 左侧 palette → 拖到画布上创建节点 → 连线组装流程
//   - sopStep: 流程步骤; 一条链 = 期望经过顺序
//   - sopEnd : 结束判定; 一般放在链尾, 决定工序何时算结束
// 保存前要求目标、步骤、入口、出口和结束判定都完整，避免运行时猜测用户意图。
//
// 右侧栏按选中节点切显示: 选中 sopStep → 步骤参数; 选中 sopEnd → 结束判定参数; 都没选 → 占位。

const EDGE_COLOR = '#06b6d4'
const END_EDGE_COLOR = '#14b8a6'
const LOOP_COLOR = '#f59e0b'    /* 循环/自环边的醒目橙色 */
const nodeTypes = { sopStep: SopStepNode, sopEnd: SopEndStepNode }
const edgeTypes = { sopSelfLoop: SopSelfLoopEdge }

interface Props {
  availableZones: string[]            // 上游 ROI 节点里画好的区域名(步骤/终点区域都从中选)
  initial: SopFlow
  onSave: (flow: SopFlow) => void
  onClose: () => void
}

type EditableNumber = number | ''
type StepData = { zoneName: string; enter_sec: EditableNumber; dwell_min_sec: EditableNumber; dwell_max_sec: EditableNumber; isEntry?: boolean }
type EndData  = { end_mode: SopEndMode; reset_sec: EditableNumber; end_zone: string; end_dwell_sec: EditableNumber; total_min_sec: EditableNumber; total_max_sec: EditableNumber }

const DEFAULT_END_DATA: EndData = { end_mode: 'leave', reset_sec: 5, end_zone: '', end_dwell_sec: 0, total_min_sec: 0, total_max_sec: 0 }

/* number input 受控时，Number('') 会立刻变成 0，导致用户无法清空后重输。
 * 编辑态保留空字符串，保存流程时再按各字段默认值转回 number。 */
const editableNumber = (raw: string): EditableNumber => raw === '' ? '' : Number(raw)
const savedNumber = (value: EditableNumber | undefined, fallback: number): number => {
  if (value === '' || value === undefined) return fallback
  const number = Number(value)
  return Number.isFinite(number) ? number : fallback
}

// Palette chip 定义(子画布版, 沿用主画布同一套类名 .palette-chip + .{cls})
// 起点 = isEntry 的步骤(一条路线的第一步); 步骤 = 普通步骤。两者同为 sopStep 节点。
const PALETTE: { type: 'sopStart' | 'sopStep' | 'sopEnd'; label: string; icon: string; cls: string }[] = [
  { type: 'sopStart', label: '起点',     icon: '🚩', cls: 'sopstart' },
  { type: 'sopStep',  label: '步骤',     icon: '📦', cls: 'sop'    },
  { type: 'sopEnd',   label: '结束判定', icon: '🏁', cls: 'sopend' },
]

let _sid = 0
const newId = (prefix: string) => `${prefix}-${Date.now()}-${++_sid}`

const mkEdge = (source: string, target: string, color = EDGE_COLOR): Edge => {
  const selfLoop = source === target
  // 自环 = 默认就是"循环边"形态(醒目橙色 + ↻ 标签提示双击配次数)
  if (selfLoop) {
    return {
      id: `e-${source}-${target}-${++_sid}`, source, target,
      sourceHandle: 'out', targetHandle: 'in',
      type: 'sopSelfLoop',
      label: '↻ ?',
      labelStyle: { fill: '#fef3c7', fontWeight: 600, fontSize: 11 },
      labelBgStyle: { fill: '#7c2d12', stroke: LOOP_COLOR },
      style: { stroke: LOOP_COLOR, strokeWidth: 3 },
      markerEnd: { type: MarkerType.ArrowClosed, color: LOOP_COLOR },
      data: {},
    }
  }
  // 非自环 step↔step 边: 都用 sopSelfLoop (SmartEdge) — 内部按 source/target 相对位置选路径,
  // 反向边自动走"上方绕弧"避开节点, 不再让 ReactFlow 默认贝塞尔穿过节点的难看路径。
  return {
    id: `e-${source}-${target}-${++_sid}`, source, target,
    sourceHandle: 'out', targetHandle: 'in', type: 'sopSelfLoop',
    style: { stroke: color, strokeWidth: 2 },
    markerEnd: { type: MarkerType.ArrowClosed, color },
  }
}

// 节点 + 连线 → 步骤顺序(节点 id): 优先按连线成链, 否则按 x 从左到右。
// 仅在 sopStep 节点之间排序; sopEnd 不参与。
function deriveOrder(stepNodes: Node[], edges: Edge[]): string[] {
  const ids = stepNodes.map(n => n.id)
  if (ids.length <= 1) return ids
  const next = new Map<string, string>()
  const indeg = new Map<string, number>()
  ids.forEach(id => indeg.set(id, 0))
  edges.forEach(e => {
    if (next.has(e.source) || !ids.includes(e.source) || !ids.includes(e.target)) return
    next.set(e.source, e.target)
    indeg.set(e.target, (indeg.get(e.target) ?? 0) + 1)
  })
  const heads = ids.filter(id => (indeg.get(id) ?? 0) === 0)
  if (heads.length === 1) {
    const chain: string[] = []
    const seen = new Set<string>()
    let cur: string | undefined = heads[0]
    while (cur && !seen.has(cur)) { chain.push(cur); seen.add(cur); cur = next.get(cur) }
    if (chain.length === ids.length) return chain
  }
  const posX = new Map(stepNodes.map(n => [n.id, n.position.x]))
  return [...ids].sort((a, b) => (posX.get(a) ?? 0) - (posX.get(b) ?? 0))
}

function Inner({ availableZones, initial, onSave, onClose }: Props) {
  const [nodes, setNodes, onNodesChange] = useNodesState<Node>([])
  const [edges, setEdges, onEdgesChange] = useEdgesState<Edge>([])
  const [target, setTarget] = useState(initial.target_label ?? '')
  const [triggerMode, setTriggerMode] = useState<SopTriggerMode>(initial.trigger_mode ?? 'auto')
  const [triggerMandatory, setTriggerMandatory] = useState(initial.trigger_mandatory ?? false)
  const [reportNormal, setReportNormal] = useState(initial.report_normal ?? false)
  const { screenToFlowPosition } = useReactFlow()
  const [toast, setToast] = useState<string | null>(null)
  const toastTimer = useRef<number | null>(null)
  const flash = (msg: string) => {
    if (toastTimer.current) { window.clearTimeout(toastTimer.current); toastTimer.current = null }
    setToast(msg)
    toastTimer.current = window.setTimeout(() => { setToast(null); toastTimer.current = null }, 1400)
  }

  // ─── 最新 nodes/edges ref(给键盘闭包用,避免 useCallback 频繁重建) ───
  const nodesRef = useRef<Node[]>([])
  const edgesRef = useRef<Edge[]>([])
  useEffect(() => { nodesRef.current = nodes }, [nodes])
  useEffect(() => { edgesRef.current = edges }, [edges])

  // ─── 历史栈(撤销/恢复); 用 ref + debounce snapshot, 避免每次拖动都入栈 ───
  type Snap = { nodes: Node[]; edges: Edge[] }
  const historyRef = useRef<{ stack: Snap[]; idx: number; restoring: boolean }>({ stack: [], idx: -1, restoring: false })
  const snapSig = (n: Node[], e: Edge[]): string => JSON.stringify({
    n: n.map(x => ({ i: x.id, t: x.type, x: Math.round(x.position.x), y: Math.round(x.position.y), d: x.data })),
    e: e.map(x => ({ i: x.id, s: x.source, t: x.target, d: x.data, l: x.label })),
  })
  useEffect(() => {
    if (historyRef.current.restoring) { historyRef.current.restoring = false; return }
    const tm = window.setTimeout(() => {
      const h = historyRef.current
      const snap: Snap = {
        nodes: nodes.map(n => ({ ...n, data: { ...(n.data ?? {}) } })),
        edges: edges.map(e => ({ ...e, data: { ...(e.data ?? {}) } })),
      }
      const top = h.stack[h.idx]
      if (top && snapSig(top.nodes, top.edges) === snapSig(snap.nodes, snap.edges)) return
      h.stack = [...h.stack.slice(0, h.idx + 1), snap].slice(-200)
      h.idx = h.stack.length - 1
    }, 220)
    return () => window.clearTimeout(tm)
  }, [nodes, edges])

  const restoreSnap = (s: Snap) => {
    historyRef.current.restoring = true
    setNodes(s.nodes.map(n => ({ ...n, data: { ...(n.data ?? {}) } })))
    setEdges(s.edges.map(e => ({ ...e, data: { ...(e.data ?? {}) } })))
  }
  const undo = useCallback((): boolean => {
    const h = historyRef.current
    if (h.idx <= 0) return false
    h.idx -= 1
    restoreSnap(h.stack[h.idx])
    return true
  }, [])
  const redo = useCallback((): boolean => {
    const h = historyRef.current
    if (h.idx >= h.stack.length - 1) return false
    h.idx += 1
    restoreSnap(h.stack[h.idx])
    return true
  }, [])

  // ─── 剪贴板: 仅复制 sopStep(结束判定唯一/不复制) ───
  const clipboardRef = useRef<{ nodes: Node[]; edges: Edge[] }>({ nodes: [], edges: [] })
  const copySelection = useCallback((): number => {
    const sel = nodesRef.current.filter(n => n.selected && n.type === 'sopStep')
    if (sel.length === 0) return 0
    const ids = new Set(sel.map(n => n.id))
    const innerEdges = edgesRef.current.filter(e => ids.has(e.source) && ids.has(e.target))
    clipboardRef.current = {
      nodes: sel.map(n => ({ ...n, data: { ...(n.data ?? {}) } })),
      edges: innerEdges.map(e => ({ ...e, data: { ...(e.data ?? {}) } })),
    }
    return sel.length
  }, [])
  const deleteSelection = useCallback((): number => {
    const selIds = new Set(nodesRef.current.filter(n => n.selected && n.type === 'sopStep').map(n => n.id))
    if (selIds.size === 0) return 0
    setNodes(ns => ns.filter(n => !selIds.has(n.id)))
    setEdges(es => es.filter(e => !selIds.has(e.source) && !selIds.has(e.target)))
    return selIds.size
  }, [setNodes, setEdges])
  const pasteClipboard = useCallback((): number => {
    const clip = clipboardRef.current
    if (clip.nodes.length === 0) return 0
    const idMap = new Map<string, string>()
    const offset = 40
    const newNodes: Node[] = clip.nodes.map(n => {
      const nid = newId('step')
      idMap.set(n.id, nid)
      return {
        ...n, id: nid, selected: true,
        position: { x: n.position.x + offset, y: n.position.y + offset },
        data: { ...(n.data ?? {}) },
      }
    })
    const newEdges: Edge[] = clip.edges.map(e => {
      const sid = idMap.get(e.source)!, tid = idMap.get(e.target)!
      return { ...e, id: `e-${sid}-${tid}-${++_sid}`, source: sid, target: tid, selected: false }
    })
    setNodes(ns => [...ns.map(n => ({ ...n, selected: false })), ...newNodes])
    setEdges(es => [...es, ...newEdges])
    return newNodes.length
  }, [setNodes, setEdges])

  // ─── 弹窗内键盘快捷键: Ctrl+C/X/V/Z/Y; 输入框打字时让位 ───
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (!(e.ctrlKey || e.metaKey) || e.altKey) return
      const t = e.target as HTMLElement | null
      if (t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' ||
                t.tagName === 'SELECT' || t.isContentEditable)) return
      const k = e.key.toLowerCase()
      if (k === 'z' && !e.shiftKey) { if (undo()) { e.preventDefault(); flash('已撤销') } return }
      if (k === 'y' || (k === 'z' && e.shiftKey)) { if (redo()) { e.preventDefault(); flash('已恢复') } return }
      if (e.shiftKey) return
      if (k === 'c') { const n = copySelection(); if (n) { e.preventDefault(); flash(`已复制 ${n} 个步骤`) } }
      else if (k === 'x') { const n = copySelection(); if (n) { deleteSelection(); e.preventDefault(); flash(`已剪切 ${n} 个步骤`) } }
      else if (k === 'v') { const n = pasteClipboard(); if (n) { e.preventDefault(); flash(`已粘贴 ${n} 个步骤`) } }
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [copySelection, deleteSelection, pasteClipboard, undo, redo])

  // 初始化子画布: 步骤节点(优先用 step.x/step.y, 否则按 idx 落网格) + DAG 边 + 结束判定节点(同理)
  useEffect(() => {
    const gridX = (i: number) => 120 + (i % 5) * 190
    const gridY = (i: number) => 80  + Math.floor(i / 5) * 120
    const initEntriesSet = new Set(initial.entries)
    const isEntryOf = (i: number) => initEntriesSet.has(i)
    const stepNs: Node[] = (initial.steps ?? []).map((s, i) => ({
      id: newId('step'), type: 'sopStep',
      position: {
        x: Number.isFinite(s.x as number) ? Number(s.x) : gridX(i),
        y: Number.isFinite(s.y as number) ? Number(s.y) : gridY(i),
      },
      data: { zoneName: s.zoneName, enter_sec: s.enter_sec, dwell_min_sec: s.dwell_min_sec, dwell_max_sec: s.dwell_max_sec, isEntry: isEntryOf(i) } as StepData,
    }))
    const endN: Node = {
      id: newId('end'), type: 'sopEnd',
      position: {
        x: Number.isFinite(initial.end_x as number) ? Number(initial.end_x) : gridX(stepNs.length),
        y: Number.isFinite(initial.end_y as number) ? Number(initial.end_y) : gridY(stepNs.length),
      },
      data: {
        end_mode: initial.end_mode ?? 'leave',
        reset_sec: initial.reset_sec ?? 5,
        end_zone: initial.end_zone ?? '',
        end_dwell_sec: initial.end_dwell_sec ?? 0,
        total_min_sec: initial.total_min_sec ?? 0,
        total_max_sec: initial.total_max_sec ?? 0,
      } as EndData,
    }
    setNodes([...stepNs, endN])

    const es: Edge[] = []
    const idxToId = (i: number): string | undefined => stepNs[i]?.id
    const initEdges = initial.edges
    const initExits = initial.exits
    const initLimits = initial.edge_limits
    // (src,dst) → {min, max} 快速查
    const limitOf = new Map<string, { min: number; max: number }>()
    for (const l of initLimits) limitOf.set(`${l.src}-${l.dst}`, { min: l.min, max: l.max })
    for (const [a, b] of initEdges) {
      const sid = idxToId(a), tid = idxToId(b)
      if (!sid || !tid) continue
      const lim = limitOf.get(`${a}-${b}`)
      const hasLimit = !!lim && (lim.min > 0 || lim.max > 0)
      const selfLoop = sid === tid
      if (hasLimit || selfLoop) {
        const lbl = hasLimit ? `↻ ${lim!.min || '∞'}…${lim!.max || '∞'}` : '↻ ?'
        es.push({
          id: `e-${sid}-${tid}-${++_sid}`, source: sid, target: tid,
          sourceHandle: 'out', targetHandle: 'in',
          type: 'sopSelfLoop',
          label: lbl,
          labelStyle: { fill: '#fef3c7', fontWeight: 600, fontSize: 11 },
          labelBgPadding: [6, 3] as [number, number],
          labelBgBorderRadius: 4,
          labelBgStyle: { fill: '#7c2d12', stroke: LOOP_COLOR },
          style: { stroke: LOOP_COLOR, strokeWidth: 3 },
          markerEnd: { type: MarkerType.ArrowClosed, color: LOOP_COLOR },
          data: hasLimit ? { loop_min: lim!.min, loop_max: lim!.max } : {},
        })
      } else {
        es.push(mkEdge(sid, tid))
      }
    }
    for (const ei of initExits) {
      const sid = idxToId(ei)
      if (sid) es.push(mkEdge(sid, endN.id, END_EDGE_COLOR))
    }
    setEdges(es)
  }, []) // eslint-disable-line react-hooks/exhaustive-deps

  // 仅 sopStep 节点参与序号
  const stepNodes = useMemo(() => nodes.filter(n => n.type === 'sopStep'), [nodes])
  const orderedIds = useMemo(() => deriveOrder(stepNodes, edges), [stepNodes, edges])

  // 按【路线】编号(不是全局): 每个起点(isEntry)= 第1步, 沿边 BFS 逐步 +1。
  // 同一步被多条路线经过取最小级数; 不可达步骤回退到全局序号。这样两条路线的起点都显示"第1步"。
  const routeNum = useMemo(() => {
    const adj = new Map<string, string[]>()
    stepNodes.forEach(n => adj.set(n.id, []))
    edges.forEach(e => { if (adj.has(e.source) && adj.has(e.target)) adj.get(e.source)!.push(e.target) })
    const entryIds = stepNodes.filter(n => (n.data as StepData).isEntry).map(n => n.id)
    const starts = entryIds
    const num = new Map<string, number>()
    for (const s of starts) {
      const queue: [string, number][] = [[s, 1]]
      while (queue.length) {
        const [id, lvl] = queue.shift()!
        const prev = num.get(id)
        if (prev !== undefined && prev <= lvl) continue   // 已有更短(或相等)路径 → 不再扩展(防环)
        num.set(id, lvl)
        for (const nxt of adj.get(id) ?? []) queue.push([nxt, lvl + 1])
      }
    }
    // 不可达步骤(孤立/未连起点)回退到全局序号, 避免显示空
    stepNodes.forEach(n => { if (!num.has(n.id)) num.set(n.id, orderedIds.indexOf(n.id) + 1) })
    return num
  }, [stepNodes, edges, orderedIds])

  // 给步骤节点注入【按路线】显示序号; sopEnd 节点原样
  const flowNodes = useMemo(
    () => nodes.map(n =>
      n.type === 'sopStep'
        ? { ...n, data: { ...n.data, seq: routeNum.get(n.id) ?? 0, triggerMode } }
        : n,
    ),
    [nodes, routeNum, triggerMode],
  )

  // ─── Drag-from-palette ───
  // 用独立的 dataTransfer key (application/reactflow-sop), 避免和主画布的
  // application/reactflow 冲突 —— 否则从子画布拖出的步骤节点会同时被主画布的 onDrop
  // 接到, 在主画布上多出一个 type=sopStep 的"白色窄框"(主画布 nodeTypes 没注册它)。
  const onDragOver = useCallback((e: React.DragEvent) => {
    e.preventDefault()
    e.dataTransfer.dropEffect = 'move'
  }, [])

  const onDrop = useCallback((e: React.DragEvent) => {
    e.preventDefault()
    const dragType = e.dataTransfer.getData('application/reactflow-sop')
    if (dragType !== 'sopStart' && dragType !== 'sopStep' && dragType !== 'sopEnd') return
    // 已确认是 SOP 节点后由子画布消费，避免事件继续影响外层主画布。
    e.stopPropagation()
    const position = screenToFlowPosition({ x: e.clientX, y: e.clientY })
    // 起点 = isEntry 的步骤; 起点和步骤都是 sopStep 节点
    const realType = dragType === 'sopEnd' ? 'sopEnd' : 'sopStep'
    const id = newId(realType)
    const data = dragType === 'sopEnd'
      ? { ...DEFAULT_END_DATA } as EndData
      : { zoneName: availableZones[0] ?? '', enter_sec: DEFAULT_STEP_ENTER, dwell_min_sec: DEFAULT_STEP_DWELL, dwell_max_sec: DEFAULT_STEP_DWELL_MAX, isEntry: dragType === 'sopStart' } as StepData
    setNodes(ns => [
      ...ns.map(n => ({ ...n, selected: false })),
      { id, type: realType, position, data, selected: true } as Node,
    ])
  }, [screenToFlowPosition, availableZones, setNodes])

  const onConnect = useCallback((p: Connection) => {
    if (!p.source || !p.target) return
    const src = nodes.find(n => n.id === p.source)
    const tgt = nodes.find(n => n.id === p.target)
    // sopEnd 没有 source handle, 不能从它连出
    if (src?.type === 'sopEnd') return
    // step → 🏁结束判定: 出口(青绿边); 其余: 普通(青色)
    const color = tgt?.type === 'sopEnd' ? END_EDGE_COLOR : EDGE_COLOR
    setEdges(eds => {
      // 允许任意拓扑 (分岔/汇合/环/自环); 仅去重 — 同 source-target 对只保留一条
      const dup = eds.some(e => e.source === p.source && e.target === p.target)
      if (dup) return eds
      return addEdge(mkEdge(p.source!, p.target!, color), eds)
    })
  }, [setEdges, nodes])

  // 应用循环次数到边 (右侧栏 onChange 实时调用; 自环边也用这个 path 更新样式/label)
  const applyLoopLimit = (edgeId: string, mn: EditableNumber, mx: EditableNumber) => {
    const mnValue = savedNumber(mn, 0)
    const mxValue = savedNumber(mx, 0)
    setEdges(eds => eds.map(e => {
      if (e.id !== edgeId) return e
      const selfLoop = e.source === e.target
      const hasLimit = mnValue > 0 || mxValue > 0
      // 自环: 永远橙色, 标签随配置变化 ("↻ ?" 或 "↻ N…M")
      // 非自环: 配了 limit → 橙色加粗 + 标签; 没配 → 普通青色
      const showAsLoop = selfLoop || hasLimit
      const lbl = hasLimit ? `↻ ${mnValue || '∞'}…${mxValue || '∞'}` : (selfLoop ? '↻ ?' : undefined)
      const color = showAsLoop ? LOOP_COLOR : EDGE_COLOR
      return {
        ...e,
        label: lbl,
        labelStyle: { fill: '#fef3c7', fontWeight: 600, fontSize: 11 },
        labelBgPadding: [6, 3] as [number, number],
        labelBgBorderRadius: 4,
        labelBgStyle: { fill: '#7c2d12', stroke: LOOP_COLOR },
        style: { stroke: color, strokeWidth: showAsLoop ? 3 : 2 },
        markerEnd: { type: MarkerType.ArrowClosed, color },
        data: { loop_min: mn, loop_max: mx },
      }
    }))
  }

  const sel       = nodes.find(n => n.selected) ?? null
  const isEndSel  = sel?.type === 'sopEnd'
  const isStepSel = sel?.type === 'sopStep'
  const selStepData = (isStepSel ? sel?.data : null) as StepData | null
  const selEndData  = (isEndSel  ? sel?.data : null) as EndData  | null
  const selIdx  = sel ? (routeNum.get(sel.id) ?? 0) - 1 : -1   // 按路线序号(显示用)

  // 选中的边(用于循环次数配置); 节点选中优先级高于边 — 同时选中时仍走节点表单
  const selEdgeRaw = edges.find(e => e.selected) ?? null
  const selEdgeTgt = selEdgeRaw ? nodes.find(n => n.id === selEdgeRaw.target) : null
  // 只有 step↔step 边参与循环配置: 排除 step→sopEnd(出口标记)
  const isEdgeSel = !sel && !!selEdgeRaw && selEdgeTgt?.type !== 'sopEnd'
  const selEdgeData = (isEdgeSel ? selEdgeRaw?.data : null) as { loop_min?: EditableNumber; loop_max?: EditableNumber } | null
  const selEdgeIsSelfLoop = !!selEdgeRaw && selEdgeRaw.source === selEdgeRaw.target
  const selEdgeSrcIdx = selEdgeRaw ? (routeNum.get(selEdgeRaw.source) ?? 0) - 1 : -1   // 按路线序号(显示用)
  const selEdgeDstIdx = selEdgeRaw ? (routeNum.get(selEdgeRaw.target) ?? 0) - 1 : -1

  const updateSelStep = (patch: Partial<StepData>) => {
    if (!sel) return
    setNodes(ns => ns.map(n => n.id === sel.id ? { ...n, data: { ...(n.data as StepData), ...patch } } : n))
  }
  const updateSelEnd = (patch: Partial<EndData>) => {
    if (!sel) return
    setNodes(ns => ns.map(n => n.id === sel.id ? { ...n, data: { ...(n.data as EndData), ...patch } } : n))
  }

  const handleSave = () => {
    // 步骤按 orderedIds 序列化，idx 是结构化边的 step 引用。
    const orderedStepNodes = orderedIds.map(id => nodes.find(n => n.id === id)!)
                                        .filter(n => n && (n.data as StepData).zoneName)
    const steps: SopStep[] = orderedStepNodes.map(n => {
      const dd = n.data as StepData
      return {
        zoneName: String(dd.zoneName ?? '').trim(),
        enter_sec: savedNumber(dd.enter_sec, DEFAULT_STEP_ENTER),
        dwell_min_sec: savedNumber(dd.dwell_min_sec, DEFAULT_STEP_DWELL),
        dwell_max_sec: savedNumber(dd.dwell_max_sec, DEFAULT_STEP_DWELL_MAX),
        x: Math.round(n.position.x),
        y: Math.round(n.position.y),
      }
    })

    // 起点 = isEntry 的步骤索引(按 orderedStepNodes 的顺序 = step idx)
    const entryList = orderedStepNodes
      .map((n, i) => ((n.data as StepData).isEntry ? i : -1))
      .filter(i => i >= 0)

    // 节点 id → step idx 映射。
    const idToIdx = new Map<string, number>()
    orderedStepNodes.forEach((n, i) => idToIdx.set(n.id, i))

    // 边分类: sopStep↔sopStep = edges; step→🏁结束判定 = exits
    const dagEdges: [number, number][] = []
    const exitSet = new Set<number>()
    const edgeLimits: { src: number; dst: number; min: number; max: number }[] = []
    const endNodeId = nodes.find(n => n.type === 'sopEnd')?.id
    for (const e of edges) {
      const a = idToIdx.get(e.source)
      const b = idToIdx.get(e.target)
      // 允许 a===b 自环 (后端语义: 自环 = "重新进入"次数计数)
      if (a !== undefined && b !== undefined) {
        dagEdges.push([a, b])
        const d = e.data as { loop_min?: number; loop_max?: number } | undefined
        const mn = Number(d?.loop_min ?? 0), mx = Number(d?.loop_max ?? 0)
        if (mn > 0 || mx > 0) edgeLimits.push({ src: a, dst: b, min: mn, max: mx })
      } else if (a !== undefined && e.target === endNodeId) {
        // step → 🏁 结束判定: 标记该 step 为出口(漏检 BFS 用它作为目标节点)
        exitSet.add(a)
      }
    }
    const exitList = Array.from(exitSet).sort((x, y) => x - y)

    const endNode = nodes.find(n => n.type === 'sopEnd')
    const e = endNode?.data as EndData | undefined
    if (!target.trim()) { flash('请设置目标类别'); return }
    if (steps.length === 0) { flash('请至少添加一个步骤'); return }
    if (entryList.length === 0) { flash('请至少标记一个起点'); return }
    if (exitList.length === 0) { flash('请把至少一个步骤连接到结束判定'); return }
    if (steps.length > 1 && dagEdges.length === 0) { flash('多个步骤之间必须显式连线'); return }
    if (!endNode || !e) { flash('请添加结束判定节点'); return }
    if (e.end_mode === 'endzone' && !e.end_zone.trim()) { flash('终点区域模式必须选择区域'); return }
    const totalMin = Math.max(0, savedNumber(e.total_min_sec, 0))
    const totalMax = Math.max(0, savedNumber(e.total_max_sec, 0))
    if (totalMax > 0 && totalMin > totalMax) { flash('总耗时下限不能大于上限'); return }

    onSave({
      target_label: target.trim(),
      reset_sec: savedNumber(e.reset_sec, 5),
      end_mode: e.end_mode === 'endzone' ? 'endzone' : (e.end_mode === 'trigger' ? 'trigger' : 'leave'),
      end_zone: e.end_mode === 'endzone' ? e.end_zone : '',
      end_dwell_sec: e.end_mode === 'endzone' ? Math.max(0, savedNumber(e.end_dwell_sec, 0)) : 0,
      total_min_sec: totalMin,
      total_max_sec: totalMax,
      trigger_mode: triggerMode,
      trigger_mandatory: triggerMandatory,
      report_normal: reportNormal,
      steps,
      edges: dagEdges,
      entries: entryList,
      exits: exitList,
      edge_limits: edgeLimits,
      end_x: Math.round(endNode.position.x),
      end_y: Math.round(endNode.position.y),
    })
    onClose()
  }

  return (
    <div className="sop-fm-overlay">
      <div className="sop-fm-dialog">
        {toast && <div className="sop-fm-toast">{toast}</div>}
        <div className="sop-fm-header">
          <span className="sop-fm-title">🧭 SOP 流程配置</span>
          <label className="sop-fm-inline">目标类别
            <input value={target} onChange={e => setTarget(e.target.value)} placeholder="如 person" />
          </label>
          <button className="sop-fm-close" onClick={onClose}>✕</button>
        </div>

        <div className="sop-fm-body">
          {/* ─── 左侧 palette ─── */}
          <div className="sop-fm-palette">
            <div className="sop-fm-palette-title">节点</div>
            {PALETTE.map(p => (
              <div
                key={p.type}
                className={`palette-chip ${p.cls}`}
                draggable
                onDragStart={e => {
                  // 独立 key, 与主画布的 application/reactflow 隔离
                  e.dataTransfer.setData('application/reactflow-sop', p.type)
                  e.dataTransfer.effectAllowed = 'move'
                }}
                title={`拖到画布上 = 新增${p.label}`}
              >
                <span>{p.icon}</span><span>{p.label}</span>
              </div>
            ))}
            <div className="sop-fm-palette-hint">
              从这里拖出节点 → 连成链 = 流程顺序<br /><br />
              步骤链末端连「🏁 结束判定」决定工序何时算结束。
            </div>
          </div>

          {/* ─── 中间子画布 ─── */}
          <div className="sop-fm-flow">
            <ReactFlow
              nodes={flowNodes} edges={edges}
              onNodesChange={onNodesChange} onEdgesChange={onEdgesChange}
              onConnect={onConnect} nodeTypes={nodeTypes} edgeTypes={edgeTypes}
              onDrop={onDrop} onDragOver={onDragOver}
              selectionOnDrag={true}
              selectionMode={SelectionMode.Partial}
              panOnDrag={[1, 2]}
              deleteKeyCode="Delete" proOptions={{ hideAttribution: true }} fitView
            >
              <Background color="#2e3352" gap={20} size={1} />
              <Controls />
              <Panel position="top-center">
                <div className="sop-fm-hint">
                  {availableZones.length === 0
                    ? '上游还没有区域 — 先在主画布的「ROI 区域」节点里绘制并命名区域, 再来这里编排'
                    : '从左侧拖步骤/结束判定 → 连成链 = 期望经过顺序'}
                </div>
              </Panel>
            </ReactFlow>
          </div>

          {/* ─── 右侧参数栏 ─── */}
          <div className="sop-fm-side">
            {isEndSel && selEndData ? (
              <>
                <div className="sop-fm-side-title">🏁 结束判定</div>
                <label className="sop-fm-field">结束方式
                  <select value={selEndData.end_mode}
                          onChange={e => updateSelEnd({ end_mode: e.target.value as SopEndMode })}>
                    <option value="leave">离场超时</option>
                    <option value="endzone">终点区域</option>
                    <option value="trigger">外部触发</option>
                  </select>
                </label>
                {selEndData.end_mode === 'endzone' && (
                  <>
                    <label className="sop-fm-field">终点区域
                      <select value={selEndData.end_zone}
                              onChange={e => updateSelEnd({ end_zone: e.target.value })}>
                        <option value="">（选择区域）</option>
                        {availableZones.map(z => <option key={z} value={z}>{z}</option>)}
                      </select>
                    </label>
                    <label className="sop-fm-field">终点停留阈值(s)
                      <input type="number" step="0.5" min="0" value={selEndData.end_dwell_sec}
                             onChange={e => updateSelEnd({ end_dwell_sec: editableNumber(e.target.value) })} />
                    </label>
                  </>
                )}
                <label className="sop-fm-field">{selEndData.end_mode !== 'leave' ? '离场兜底(s)' : '离场超时(s)'}
                  <input type="number" step="0.5" min="0" value={selEndData.reset_sec}
                         onChange={e => updateSelEnd({ reset_sec: editableNumber(e.target.value) })} />
                </label>
                <div className="sop-fm-side-hint">
                  {selEndData.end_mode === 'endzone'
                    ? '目标在终点区域连续停留达到阈值 → 工序结束；中途离开会重新计时。阈值 0 = 进入确认后立即结束；离场超过兜底时长也会结束。'
                    : selEndData.end_mode === 'trigger'
                    ? '收到外部结束触发信号 → 工序结束; 离场超过兜底时长 → 也判结束(防卡死)。可通过按钮、PLC、扫码枪等发送信号。'
                    : '目标持续检测不到超过此时长 → 工序结束。'}
                </div>

                {/* ─── 总耗时上下限 (本轮工序整体约束) ─── */}
                <div className="sop-fm-side-title" style={{ marginTop: 14 }}>⏱ 总耗时</div>
                <label className="sop-fm-field">下限(s, 0=不限)
                  <input type="number" step="0.5" min="0" value={selEndData.total_min_sec}
                         onChange={e => updateSelEnd({ total_min_sec: editableNumber(e.target.value) })} />
                </label>
                <label className="sop-fm-field">上限(s, 0=不限)
                  <input type="number" step="0.5" min="0" value={selEndData.total_max_sec}
                         onChange={e => updateSelEnd({ total_max_sec: editableNumber(e.target.value) })} />
                </label>
                <div className="sop-fm-side-hint">
                  本轮总耗时(从进入第 1 步开始计时) &lt; 下限 → "总耗时不足"(赶工); &gt; 上限 → "总耗时超时"(卡壳)。两者都填 0 = 不检查。
                </div>

                <div className="sop-fm-side-title" style={{ marginTop: 14 }}>📤 结果上报</div>
                <label className="sop-fm-field" style={{ display: 'flex', flexDirection: 'row', alignItems: 'center', gap: 8, cursor: 'pointer' }}>
                  <input type="checkbox" checked={reportNormal}
                         onChange={e => setReportNormal(e.target.checked)} />
                  <span>上报正常工序结果</span>
                </label>
                <div className="sop-fm-side-hint">
                  开启后，仅在本轮工序正式结束、到达合法出口且没有任何违规时上报一次。正常结果先进入发件箱，
                  只向已连接的 Dify 工作流发送业务 JSON，不生成图片或视频；Dify 开始节点的文件变量需允许为空。违规结果仍按原有媒体策略上报。
                </div>
              </>
            ) : isStepSel && selStepData ? (
              <>
                <div className="sop-fm-side-title">
                  {selStepData.isEntry ? '🚩 起点' : '步骤配置'}{selIdx >= 0 ? ` · 第 ${selIdx + 1} 步` : ''}
                </div>
                {selStepData.isEntry && (
                  <>
                    <label className="sop-fm-field">触发方式
                      <select value={triggerMode} onChange={e => setTriggerMode(e.target.value as SopTriggerMode)}>
                        <option value="auto">自动检测</option>
                        <option value="external">外部触发</option>
                      </select>
                    </label>
                    {triggerMode === 'external' && (
                      <label className="sop-fm-field" style={{ display: 'flex', alignItems: 'center', gap: 8, cursor: 'pointer' }}>
                        <input type="checkbox" checked={triggerMandatory}
                               onChange={e => setTriggerMandatory(e.target.checked)} />
                        <span>触发必须</span>
                      </label>
                    )}
                    <div className="sop-fm-side-hint">
                      {triggerMode === 'external'
                        ? (triggerMandatory
                            ? '必须先收到外部触发信号，目标才能进入区域。未经触发而进入 → 报违规告警。'
                            : '目标进入起点区域后不会自动开始，需等待外部信号(sop_trigger)触发。')
                        : '目标进入起点区域即自动开始本轮SOP检测。'}
                    </div>
                  </>
                )}
                <label className="sop-fm-field">区域
                  <select value={selStepData.zoneName ?? ''} onChange={e => updateSelStep({ zoneName: e.target.value })}>
                    <option value="">（未选择）</option>
                    {availableZones.map(z => <option key={z} value={z}>{z}</option>)}
                  </select>
                </label>
                <label className="sop-fm-field">进入确认(s)
                  <input type="number" step="0.1" min="0" value={selStepData.enter_sec}
                    onChange={e => updateSelStep({ enter_sec: editableNumber(e.target.value) })} />
                </label>
                <label className="sop-fm-field">最小停留(s)
                  <input type="number" step="0.5" min="0" value={selStepData.dwell_min_sec}
                    onChange={e => updateSelStep({ dwell_min_sec: editableNumber(e.target.value) })} />
                </label>
                <label className="sop-fm-field">最大停留(s，0=不限)
                  <input type="number" step="0.5" min="0" value={selStepData.dwell_max_sec}
                    onChange={e => updateSelStep({ dwell_max_sec: editableNumber(e.target.value) })} />
                </label>
                <div className="sop-fm-side-hint">参数只作用于该步骤。停太短→「停留不足」报警；停太久→「停留超时」报警(最大填 0 = 不限)。</div>
              </>
            ) : isEdgeSel && selEdgeRaw ? (
              <>
                <div className="sop-fm-side-title sop-fm-side-title--loop">
                  ↻ 循环次数
                </div>
                <div className="sop-fm-side-hint" style={{ marginBottom: 12 }}>
                  {selEdgeIsSelfLoop
                    ? <>自环 <b>步骤 {selEdgeSrcIdx + 1}</b> → 自身</>
                    : <>边 <b>步骤 {selEdgeSrcIdx + 1}</b> → <b>步骤 {selEdgeDstIdx + 1}</b></>}
                </div>
                <label className="sop-fm-field">最少走(次, 0=不限)
                  <input type="number" min="0" step="1" value={selEdgeData?.loop_min ?? 0}
                         onChange={e => {
                           const raw = editableNumber(e.target.value)
                           const mn = raw === '' ? '' : Math.max(0, Math.round(raw))
                           const mx = selEdgeData?.loop_max ?? 0
                           applyLoopLimit(selEdgeRaw.id, mn, mx)
                         }} />
                </label>
                <label className="sop-fm-field">最多走(次, 0=不限)
                  <input type="number" min="0" step="1" value={selEdgeData?.loop_max ?? 0}
                         onChange={e => {
                           const raw = editableNumber(e.target.value)
                           const mx = raw === '' ? '' : Math.max(0, Math.round(raw))
                           const mn = selEdgeData?.loop_min ?? 0
                           applyLoopLimit(selEdgeRaw.id, mn, mx)
                         }} />
                </label>
                <button className="sop-fm-btn" style={{ marginTop: 8, width: '100%' }}
                        onClick={() => { applyLoopLimit(selEdgeRaw.id, 0, 0); flash('已清除循环约束') }}>
                  清除约束
                </button>
              </>
            ) : (
              <div className="sop-fm-side-empty">
                点画布上的步骤<br />选区域并单独配置参数<br /><br />
                点「🏁 结束判定」节点<br />配置工序何时算结束<br /><br />
                点画布上的边<br />配置循环次数 ↻
              </div>
            )}
          </div>
        </div>

        <div className="sop-fm-footer">
          <span className="sop-fm-foot-hint">🚩起点 = 一条路线的第一步(可多个/同区域,靠后续区域区分) · step→🏁 = 出口 · 单击边配循环次数 ↻ · Delete · Ctrl+C/X/V · Ctrl+Z/Y</span>
          <button className="sop-fm-btn primary" onClick={handleSave}>保存流程</button>
        </div>

      </div>
    </div>
  )
}

export default function SopFlowModal(props: Props) {
  return <ReactFlowProvider><Inner {...props} /></ReactFlowProvider>
}
