import { Handle, Position, NodeProps } from '@xyflow/react'
import './nodeStyles.css'

export default function ReportNode({ data, selected }: NodeProps) {
  const d     = data as Record<string, unknown>
  const policy = (d.report_policy && typeof d.report_policy === 'object'
    ? d.report_policy : {}) as Record<string, unknown>
  const deliveries = Array.isArray(policy.deliveries) ? policy.deliveries as Record<string, unknown>[] : []
  const delivery = deliveries[0]
  const target = delivery
    ? `${delivery.media === 'video' ? '视频' : '图片'}→${delivery.target}${delivery.profile_id ? ` (${delivery.profile_id})` : ''}`
    : '未配置投递任务'
  const color = '#ef4444'

  return (
    <div className={`rf-node rf-node-compact${selected ? ' selected' : ''}`}>
      <Handle type="target" position={Position.Left} id="report-in" />

      <div className="rf-node-header header-report">
        <span>📡</span>
        <span>上报配置</span>
        <span className="node-type-badge"
          style={{ marginLeft: 'auto', background: `${color}22`, color, border: `1px solid ${color}55` }}>
          HTTP
        </span>
      </div>
      <div className="rf-node-summary" title={target}>{target}</div>
    </div>
  )
}
