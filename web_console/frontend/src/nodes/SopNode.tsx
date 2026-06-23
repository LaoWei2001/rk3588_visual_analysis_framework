import { Handle, Position, NodeProps, useReactFlow } from '@xyflow/react'
import { useState, useEffect } from 'react'
import { createPortal } from 'react-dom'
import { useROIStore } from '../store/roiStore'
import { useSopUiStore } from '../store/sopUiStore'
import SopFlowModal from '../components/SopFlowModal'
import { type SopFlow, DEFAULT_SOP_FLOW } from '../utils/sopFlow'
import './nodeStyles.css'

// SOP 流程节点: 接在 YOLO 节点之后(model.logic-out → 本节点 logic-in), 右侧接「上报配置」。
// 模型/区域沿用上游: 检测来自 model, 区域名来自 model 连的 ROI 节点 —— 与常规通道同一套架构。
// 点「配置流程」进入流程弹窗, 配置目标 + 各步骤(选区域 + 独立参数)。
export default function SopNode({ id, data, selected }: NodeProps) {
  const flow  = { ...DEFAULT_SOP_FLOW, ...(data as Partial<SopFlow>) }
  const steps = flow.steps ?? []
  const [open, setOpen] = useState(false)
  const rf = useReactFlow()
  const zonesByNode = useROIStore(s => s.zones)
  const setFlowOpen = useSopUiStore(s => s.setFlowOpen)

  // 弹窗打开期间, 告知主画布暂停 Delete 删节点(否则按 Delete 删的是这个 SOP 节点本身)
  useEffect(() => { setFlowOpen(open); return () => setFlowOpen(false) }, [open, setFlowOpen])

  // 取上游 ROI 节点里的区域名: sop --logic-in--> model --roi-in--> roi
  const availableZones = (): string[] => {
    const edges = rf.getEdges()
    const toModel = edges.find(e => e.target === id && e.targetHandle === 'logic-in')
    if (!toModel) return []
    const toRoi = edges.find(e => e.target === toModel.source && e.targetHandle === 'roi-in')
    if (!toRoi) return []
    return (zonesByNode[toRoi.source] ?? [])
      .map(z => z.name?.trim())
      .filter((n): n is string => !!n)
  }

  const saveFlow = (f: SopFlow) =>
    rf.setNodes(ns => ns.map(n => (n.id === id ? { ...n, data: { ...f } } : n)))

  return (
    <>
      <div className={`rf-node rf-node-compact${selected ? ' selected' : ''}`} style={{ minWidth: 190 }}>
        {/* 接 YOLO 推理(model.logic-out) */}
        <Handle type="target" position={Position.Left} id="logic-in" />

        <div className="rf-node-header header-sop">
          <span>🧭</span><span>SOP 流程</span>
        </div>
        <div className="rf-node-summary" title={flow.target_label || '未设目标'}>
          目标: {flow.target_label || '未设'} · {steps.length} 步
        </div>
        <button className="node-btn full primary" onClick={() => setOpen(true)}>⚙ 配置流程</button>

        {/* 接「上报配置」(沿用既有上报节点) */}
        <Handle type="source" position={Position.Right} id="report-out" />
      </div>

      {open && createPortal(
        <SopFlowModal
          availableZones={availableZones()}
          initial={flow}
          onSave={saveFlow}
          onClose={() => setOpen(false)}
        />,
        document.body,
      )}
    </>
  )
}
