import { Handle, Position, NodeProps } from '@xyflow/react'
import { getSrcType } from '../utils/streamSource'
import './nodeStyles.css'

const TYPE_LABELS: Record<string, string> = { rtsp: 'RTSP', usb: 'USB', file: 'FILE' }
const TYPE_COLORS: Record<string, string> = { rtsp: '#3b82f6', usb: '#22c55e', file: '#f59e0b' }

export default function StreamNode({ data, selected }: NodeProps) {
  const d       = data as Record<string, unknown>
  const srcType = getSrcType(d)
  const chId    = d.channel_id != null ? Number(d.channel_id) : 0
  const addr    = srcType === 'usb'
    ? String(d.device ?? '/dev/video0')
    : String(d.url    ?? '（未配置地址）')
  const color = srcType ? (TYPE_COLORS[srcType] ?? '#6b7280') : '#ef4444'

  return (
    <div className={`rf-node rf-node-compact${selected ? ' selected' : ''}`}>
      <div className="rf-node-header header-stream">
        <span>◈</span>
        <span>视频流</span>
        <span className="node-type-badge"
          style={{ background: `${color}22`, color, border: `1px solid ${color}55` }}>
          {srcType ? (TYPE_LABELS[srcType] ?? srcType.toUpperCase()) : '未指定'}
        </span>
      </div>
      <div className="rf-node-summary" title={addr}>
        <span className="node-ch-label">Ch.{chId}</span>
        <span>{addr}</span>
      </div>
      <Handle type="source" position={Position.Right} id="stream-out" />
    </div>
  )
}
