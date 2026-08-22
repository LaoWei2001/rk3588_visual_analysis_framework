import { Handle, Position, NodeProps } from '@xyflow/react'
import './nodeStyles.css'

export default function ReportNode({ data, selected }: NodeProps) {
  const d     = data as Record<string, unknown>
  const policy = (d.report_policy && typeof d.report_policy === 'object'
    ? d.report_policy : {}) as Record<string, unknown>
  const deliveries = Array.isArray(policy.deliveries) ? policy.deliveries as Record<string, unknown>[] : []
  const delivery = deliveries[0]
  const enabled = policy.enabled !== false && delivery?.enabled !== false
  const media = delivery && Array.isArray(delivery.media) ? delivery.media as string[] : []
  const targetConfig = delivery
    ? `${delivery.contract_label || '接口模板'} · ${media.length ? media.join(', ') : '仅事件数据'}${delivery.connection_id ? ` (${delivery.connection_id})` : ''}`
    : '未配置投递任务'
  const target = enabled ? targetConfig : `已关闭 · ${targetConfig}`
  const color = enabled ? '#ef4444' : '#94a3b8'

  return (
    <div className={`rf-node rf-node-compact${selected ? ' selected' : ''}`}>
      <Handle type="target" position={Position.Left} id="report-in" />

      <div className="rf-node-header header-report">
        <span>📡</span>
        <span>上报配置</span>
        <span className="node-type-badge"
          style={{ marginLeft: 'auto', background: `${color}22`, color, border: `1px solid ${color}55` }}>
          {enabled ? 'EVENT' : 'OFF'}
        </span>
      </div>
      <div className="rf-node-summary" title={target}>{target}</div>
    </div>
  )
}
