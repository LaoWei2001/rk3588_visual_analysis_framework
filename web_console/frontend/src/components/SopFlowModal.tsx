import { useCallback, useEffect, useMemo, useState } from 'react'
import {
  ReactFlow, ReactFlowProvider, Background, Controls, Panel,
  useNodesState, useEdgesState, addEdge,
  Node, Edge, Connection, MarkerType,
} from '@xyflow/react'
import '@xyflow/react/dist/style.css'
import SopStepNode from '../nodes/SopStepNode'
import { type SopFlow, type SopStep, type SopEndMode, DEFAULT_STEP_ENTER, DEFAULT_STEP_DWELL } from '../utils/sopFlow'
import './SopFlowModal.css'

// 「SOP 流程配置」弹窗: 点 SOP 节点上的「配置流程」进来。
// 自包含、与外部解耦: 只接收 (上游 ROI 节点的)可选区域名 + 初始流程, 编辑完回吐一个 SopFlow。
// 子画布 = 步骤节点连成一条链(顺序); 右侧给选中步骤选区域 + 单独设参数。

const EDGE_COLOR = '#06b6d4'
const nodeTypes = { sopStep: SopStepNode }

interface Props {
  availableZones: string[]            // 上游 ROI 节点里画好的区域名(步骤从中选)
  initial: SopFlow
  onSave: (flow: SopFlow) => void
  onClose: () => void
}

type StepData = { zoneName: string; enter_sec: number; dwell_min_sec: number }

let _sid = 0
const newId = () => `step-${Date.now()}-${++_sid}`

const mkEdge = (source: string, target: string): Edge => ({
  id: `e-${source}-${target}`, source, target,
  sourceHandle: 'out', targetHandle: 'in', type: 'default',
  style: { stroke: EDGE_COLOR, strokeWidth: 2 },
  markerEnd: { type: MarkerType.ArrowClosed, color: EDGE_COLOR },
})

// 节点 + 连线 → 步骤顺序(节点 id): 优先按连线成链, 否则按 x 从左到右。
function deriveOrder(nodes: Node[], edges: Edge[]): string[] {
  const ids = nodes.map(n => n.id)
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
  const posX = new Map(nodes.map(n => [n.id, n.position.x]))
  return [...ids].sort((a, b) => (posX.get(a) ?? 0) - (posX.get(b) ?? 0))
}

function Inner({ availableZones, initial, onSave, onClose }: Props) {
  const [nodes, setNodes, onNodesChange] = useNodesState<Node>([])
  const [edges, setEdges, onEdgesChange] = useEdgesState<Edge>([])
  const [target, setTarget]   = useState(initial.target_label ?? '')
  const [reset, setReset]     = useState(initial.reset_sec ?? 5)
  const [endMode, setEndMode] = useState<SopEndMode>(initial.end_mode ?? 'leave')
  const [endZone, setEndZone] = useState(initial.end_zone ?? '')

  // 用初始步骤建子画布(只在打开时建一次)
  useEffect(() => {
    const ns: Node[] = (initial.steps ?? []).map((s, i) => ({
      id: newId(), type: 'sopStep',
      position: { x: 120 + (i % 5) * 190, y: 80 + Math.floor(i / 5) * 120 },
      data: { zoneName: s.zoneName, enter_sec: s.enter_sec, dwell_min_sec: s.dwell_min_sec } as StepData,
    }))
    setNodes(ns)
    const es: Edge[] = []
    for (let i = 0; i + 1 < ns.length; i++) es.push(mkEdge(ns[i].id, ns[i + 1].id))
    setEdges(es)
  }, []) // eslint-disable-line react-hooks/exhaustive-deps

  const orderedIds = useMemo(() => deriveOrder(nodes, edges), [nodes, edges])
  // 给每个节点注入显示用的序号(不进 onNodesChange, 只影响渲染)
  const flowNodes = useMemo(
    () => nodes.map(n => ({ ...n, data: { ...n.data, seq: orderedIds.indexOf(n.id) + 1 } })),
    [nodes, orderedIds],
  )

  const addStep = () => setNodes(ns => {
    const k = ns.length
    const node: Node = {
      id: newId(), type: 'sopStep', selected: true,
      position: { x: 120 + (k % 5) * 190, y: 80 + Math.floor(k / 5) * 120 },
      data: { zoneName: availableZones[0] ?? '', enter_sec: DEFAULT_STEP_ENTER, dwell_min_sec: DEFAULT_STEP_DWELL } as StepData,
    }
    return [...ns.map(n => ({ ...n, selected: false })), node]
  })

  const onConnect = useCallback((p: Connection) => {
    if (!p.source || !p.target || p.source === p.target) return
    setEdges(eds => addEdge(mkEdge(p.source!, p.target!),
      eds.filter(e => e.source !== p.source && e.target !== p.target)))
  }, [setEdges])

  const sel     = nodes.find(n => n.selected) ?? null
  const selData = (sel?.data as StepData | undefined) ?? null
  const selIdx  = sel ? orderedIds.indexOf(sel.id) : -1
  const updateSel = (patch: Partial<StepData>) => {
    if (!sel) return
    setNodes(ns => ns.map(n => n.id === sel.id ? { ...n, data: { ...(n.data as StepData), ...patch } } : n))
  }

  const handleSave = () => {
    const steps: SopStep[] = orderedIds.map(id => {
      const dd = nodes.find(n => n.id === id)!.data as StepData
      return {
        zoneName: String(dd.zoneName ?? '').trim(),
        enter_sec: Number(dd.enter_sec ?? DEFAULT_STEP_ENTER),
        dwell_min_sec: Number(dd.dwell_min_sec ?? DEFAULT_STEP_DWELL),
      }
    }).filter(s => s.zoneName)   // 丢掉没选区域的步骤
    onSave({
      target_label: target.trim(), reset_sec: Number(reset),
      end_mode: endMode, end_zone: endMode === 'endzone' ? endZone : '',
      steps,
    })
    onClose()
  }

  return (
    <div className="sop-fm-overlay">
      <div className="sop-fm-dialog">
        <div className="sop-fm-header">
          <span className="sop-fm-title">🧭 SOP 流程配置</span>
          <label className="sop-fm-inline">目标类别
            <input value={target} onChange={e => setTarget(e.target.value)} placeholder="如 person" />
          </label>
          <label className="sop-fm-inline">工序结束
            <select value={endMode} onChange={e => setEndMode(e.target.value as SopEndMode)}>
              <option value="leave">离场超时</option>
              <option value="endzone">终点区域</option>
            </select>
          </label>
          {endMode === 'endzone' && (
            <label className="sop-fm-inline">终点区域
              <select value={endZone} onChange={e => setEndZone(e.target.value)}>
                <option value="">（选择）</option>
                {availableZones.map(z => <option key={z} value={z}>{z}</option>)}
              </select>
            </label>
          )}
          <label className="sop-fm-inline">{endMode === 'endzone' ? '离场兜底(s)' : '离场超时(s)'}
            <input type="number" step="0.5" min="0" value={reset} onChange={e => setReset(Number(e.target.value))} />
          </label>
          <button className="sop-fm-close" onClick={onClose}>✕</button>
        </div>

        <div className="sop-fm-body">
          <div className="sop-fm-flow">
            <ReactFlow
              nodes={flowNodes} edges={edges}
              onNodesChange={onNodesChange} onEdgesChange={onEdgesChange}
              onConnect={onConnect} nodeTypes={nodeTypes}
              deleteKeyCode="Delete" proOptions={{ hideAttribution: true }} fitView
            >
              <Background color="#2e3352" gap={20} size={1} />
              <Controls />
              <Panel position="top-left">
                <button className="sop-fm-btn primary" onClick={addStep}>＋ 添加步骤</button>
              </Panel>
              <Panel position="top-center">
                <div className="sop-fm-hint">
                  {availableZones.length === 0
                    ? '上游还没有区域 — 先在画布的「ROI 区域」节点里绘制并命名区域, 再来这里编排'
                    : '添加步骤 → 点步骤在右侧选区域并设参数 → 连成一条链 = 期望经过顺序'}
                </div>
              </Panel>
            </ReactFlow>
          </div>

          <div className="sop-fm-side">
            {sel && selData ? (
              <>
                <div className="sop-fm-side-title">步骤配置{selIdx >= 0 ? ` · 第 ${selIdx + 1} 步` : ''}</div>
                <label className="sop-fm-field">区域
                  <select value={selData.zoneName ?? ''} onChange={e => updateSel({ zoneName: e.target.value })}>
                    <option value="">（未选择）</option>
                    {availableZones.map(z => <option key={z} value={z}>{z}</option>)}
                  </select>
                </label>
                <label className="sop-fm-field">进入确认(s)
                  <input type="number" step="0.1" min="0" value={selData.enter_sec ?? 0}
                    onChange={e => updateSel({ enter_sec: Number(e.target.value) })} />
                </label>
                <label className="sop-fm-field">最小停留(s)
                  <input type="number" step="0.5" min="0" value={selData.dwell_min_sec ?? 0}
                    onChange={e => updateSel({ dwell_min_sec: Number(e.target.value) })} />
                </label>
                <div className="sop-fm-side-hint">参数只作用于该步骤。同一区域的不同步骤可设不同停留。</div>
              </>
            ) : (
              <div className="sop-fm-side-empty">点画布上的步骤<br />选区域并单独配置参数</div>
            )}
          </div>
        </div>

        <div className="sop-fm-footer">
          <span className="sop-fm-foot-hint">同一区域可加成多个步骤(多次进入)，每步参数独立 · Delete 删步骤/连线</span>
          <button className="sop-fm-btn primary" onClick={handleSave}>保存流程</button>
        </div>
      </div>
    </div>
  )
}

export default function SopFlowModal(props: Props) {
  return <ReactFlowProvider><Inner {...props} /></ReactFlowProvider>
}
