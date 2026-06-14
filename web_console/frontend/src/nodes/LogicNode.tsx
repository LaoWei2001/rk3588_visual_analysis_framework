import { Handle, Position, NodeProps, useNodeConnections, useReactFlow } from '@xyflow/react'
import './nodeStyles.css'

export default function LogicNode({ id, data, selected }: NodeProps) {
  const d     = data as Record<string, unknown>
  const logic = String(d.logic ?? 'logic_default')

  // 该逻辑节点的输入是否直接来自「视频流」(而非 YOLO 模型) → 传统/无推理通道。
  // 用户可不放 YOLO 节点, 把视频流直连逻辑函数, 走传统 CV / 非 YOLO 算法。
  const inConns      = useNodeConnections({ id, handleType: 'target', handleId: 'logic-in' })
  const { getNode }  = useReactFlow()
  const fedByStream  = inConns.some(c => getNode(c.source)?.type === 'stream')

  return (
    <div className={`rf-node rf-node-compact${selected ? ' selected' : ''}`}>
      {/* top: ROI input — 传统通道也可直接连 ROI 区域 */}
      <Handle type="target" position={Position.Top}  id="roi-in"   />
      {/* left: 来自 YOLO 推理节点(logic-out) 或 直接来自视频流(stream-out) */}
      <Handle type="target" position={Position.Left} id="logic-in" />

      <div className="rf-node-header header-logic">
        <span>⚡</span>
        <span>逻辑函数</span>
        {fedByStream && (
          <span className="node-status-badge" style={{
            marginLeft: 'auto',
            color: '#fbbf24',
            background: 'rgba(251,191,36,0.15)',
            border: '1px solid rgba(251,191,36,0.4)',
          }} title="视频流直连逻辑函数：该通道不做 YOLO 推理，走传统 CV / 自定义算法">
            无推理
          </span>
        )}
      </div>
      <div className="rf-node-summary" title={logic}>{logic}</div>

      <Handle type="source" position={Position.Right} id="report-out" />
    </div>
  )
}
