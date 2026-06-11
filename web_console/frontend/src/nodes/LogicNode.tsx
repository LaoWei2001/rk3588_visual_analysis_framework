import { Handle, Position, NodeProps } from '@xyflow/react'
import './nodeStyles.css'

export default function LogicNode({ data, selected }: NodeProps) {
  const d     = data as Record<string, unknown>
  const logic = String(d.logic ?? 'logic_default')

  return (
    <div className={`rf-node rf-node-compact${selected ? ' selected' : ''}`}>
      <Handle type="target" position={Position.Left}  id="logic-in"   />

      <div className="rf-node-header header-logic">
        <span>⚡</span>
        <span>逻辑函数</span>
      </div>
      <div className="rf-node-summary" title={logic}>{logic}</div>

      <Handle type="source" position={Position.Right} id="report-out" />
    </div>
  )
}
