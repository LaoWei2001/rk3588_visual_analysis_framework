import { Handle, Position, NodeProps } from '@xyflow/react'
import './nodeStyles.css'

export default function ModelNode({ data, selected }: NodeProps) {
  const d = data as Record<string, unknown>
  const enabled   = d.infer_enable !== false   // YOLO 推理开关（不再是整条通道的 enable）
  const modelPath = String(d.model_path ?? '')
  const basename  = modelPath ? modelPath.split('/').pop()! : '（未配置）'

  return (
    <div className={`rf-node rf-node-compact${selected ? ' selected' : ''}`}>
      {/* top: ROI input */}
      <Handle type="target" position={Position.Top}  id="roi-in"    />
      {/* left: stream input */}
      <Handle type="target" position={Position.Left} id="stream-in" />

      <div className="rf-node-header header-model">
        <span>🧠</span>
        <span>YOLO 推理</span>
        <span className="node-status-badge" style={{
          marginLeft: 'auto',
          color:  enabled ? '#86efac' : '#94a3b8',
          background: enabled ? 'rgba(22,163,74,0.18)' : 'rgba(100,116,139,0.15)',
          border: `1px solid ${enabled ? '#16a34a55' : '#47556955'}`,
        }}>
          {enabled ? '推理开' : '推理关'}
        </span>
      </div>
      <div className="rf-node-summary" title={modelPath}>{basename}</div>

      {/* right: output to logic */}
      <Handle type="source" position={Position.Right} id="logic-out" />
    </div>
  )
}
