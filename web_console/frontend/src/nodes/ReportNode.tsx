import { Handle, Position, NodeProps } from '@xyflow/react'
import './nodeStyles.css'

const TYPE_LABEL: Record<string, string> = { server: 'HTTP', dify: 'Dify' }
const TYPE_COLOR: Record<string, string> = { server: '#ef4444', dify: '#a855f7' }

export default function ReportNode({ data, selected }: NodeProps) {
  const d     = data as Record<string, unknown>
  const rtype = (d.report_type as string) ?? 'server'
  const target = rtype === 'dify'
    ? (d.dify_prompt ? '已配置提示词' : '（未配置 Prompt）')
    : String(d.server_url ?? '（未配置地址）')
  const color = TYPE_COLOR[rtype] ?? '#6b7280'

  return (
    <div className={`rf-node rf-node-compact${selected ? ' selected' : ''}`}>
      <Handle type="target" position={Position.Left} id="report-in" />

      <div className="rf-node-header header-report">
        <span>📡</span>
        <span>上报配置</span>
        <span className="node-type-badge"
          style={{ marginLeft: 'auto', background: `${color}22`, color, border: `1px solid ${color}55` }}>
          {TYPE_LABEL[rtype] ?? rtype}
        </span>
      </div>
      <div className="rf-node-summary" title={target}>{target}</div>
    </div>
  )
}
