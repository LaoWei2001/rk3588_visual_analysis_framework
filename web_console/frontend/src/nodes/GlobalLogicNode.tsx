import { Handle, Position, NodeProps, useNodeConnections } from '@xyflow/react'
import './nodeStyles.css'

export default function GlobalLogicNode({ id, data, selected }: NodeProps) {
  const d = data as Record<string, unknown>
  const logic = String(d.logic ?? '')
  const inputs = useNodeConnections({ id, handleType: 'target', handleId: 'global-in' })

  return (
    <div className={`rf-node rf-node-compact${selected ? ' selected' : ''}`}>
      <Handle type="target" position={Position.Left} id="global-in" />

      <div className="rf-node-header header-global-logic">
        <span>⬡</span>
        <span>全局逻辑</span>
        <span className="node-status-badge" style={{ marginLeft: 'auto' }}>
          {d.enable === false ? '停用' : `${inputs.length} 路输入`}
        </span>
      </div>
      <div className="rf-node-summary" title={logic || '未选择全局逻辑'}>
        {logic || '未选择全局逻辑'}
      </div>

      <Handle type="source" position={Position.Right} id="report-out" />
    </div>
  )
}
