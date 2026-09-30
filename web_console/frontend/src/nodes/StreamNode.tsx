import { Handle, Position, NodeProps } from '@xyflow/react'
import { useEditorStore } from '../store/editorStore'
import { getSrcType } from '../utils/streamSource'
import './nodeStyles.css'

const TYPE_LABELS: Record<string, string> = { rtsp: 'RTSP', usb: 'USB', file: 'FILE' }
const TYPE_COLORS: Record<string, string> = { rtsp: '#3b82f6', usb: '#22c55e', file: '#f59e0b' }

export default function StreamNode({ data, selected }: NodeProps) {
  const d       = data as Record<string, unknown>
  const srcType = getSrcType(d)
  const chId    = d.channel_id != null ? Number(d.channel_id) : 0
  const addr    = srcType === 'usb'
    ? String(d.device ?? '/dev/video81')
    : String(d.url    ?? '（未配置地址）')
  const assets = useEditorStore(s => s.assets)
  const assetsReady = useEditorStore(s => s.assetsStatus === 'ready'
    && s.assetsForApp === s.appName)
  const videoMissing = assetsReady && srcType === 'file' && !!d.url
    && !assets.videos.includes(String(d.url))
  const color = srcType ? (TYPE_COLORS[srcType] ?? '#6b7280') : '#ef4444'
  const inferenceRoi = d.inference_roi && typeof d.inference_roi === 'object'
    ? d.inference_roi as Record<string, unknown> : null
  const roiOnly = inferenceRoi?.mode === 'roi_only'
  const roiEnhanced = inferenceRoi?.mode === 'full_plus_roi'
  const roiFiltered = inferenceRoi?.mode === 'full_frame_roi_filter'

  return (
    <div className={`rf-node rf-node-compact${selected ? ' selected' : ''}`}>
      {/* ROI 是通道级区域配置，直接归属于视频流，不依赖模型或逻辑。 */}
      <Handle type="target" position={Position.Top} id="roi-in" />
      <div className="rf-node-header header-stream">
        <span>◈</span>
        <span>视频流</span>
        <span className="node-type-badge"
          style={{ background: `${color}22`, color, border: `1px solid ${color}55` }}>
          {srcType ? (TYPE_LABELS[srcType] ?? srcType.toUpperCase()) : '未指定'}
        </span>
        {roiOnly && <span className="node-type-badge" style={{
          marginLeft: 0,
          background: 'rgba(6,182,212,0.18)', color: '#67e8f9', border: '1px solid rgba(6,182,212,0.45)',
        }}>ROI ONLY</span>}
        {roiEnhanced && <span className="node-type-badge" style={{
          marginLeft: 0, background: 'rgba(168,85,247,0.18)', color: '#d8b4fe',
          border: '1px solid rgba(168,85,247,0.45)',
        }}>FULL + ROI</span>}
        {roiFiltered && <span className="node-type-badge" style={{
          marginLeft: 0, background: 'rgba(20,184,166,0.18)', color: '#5eead4',
          border: '1px solid rgba(20,184,166,0.45)',
        }}>ROI FILTER</span>}
      </div>
      <div className={`rf-node-summary${videoMissing ? ' asset-missing' : ''}`}
        title={videoMissing ? `视频文件不存在：${addr}` : addr}>
        <span className="node-ch-label">Ch.{chId}</span>
        <span>{videoMissing ? '⚠ 视频文件不存在' : addr}</span>
      </div>
      <Handle type="source" position={Position.Right} id="stream-out" />
    </div>
  )
}
