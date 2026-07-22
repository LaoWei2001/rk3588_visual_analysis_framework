import { Handle, Position, NodeProps } from '@xyflow/react'
import './nodeStyles.css'

// 「结束判定」节点 — 仅用于「SOP 流程配置」弹窗内部的子画布, 是步骤链的尾节点。
// 与 SopStepNode 平级。没有 source handle: 永远是链尾, 任何节点都不能从它出去。
// 选中后在右侧栏编辑: end_mode / reset_sec / end_zone / total_min_sec / total_max_sec
//
// data 形状: { end_mode, reset_sec, end_zone, total_min_sec, total_max_sec }
export default function SopEndStepNode({ data, selected }: NodeProps) {
  const d = data as {
    end_mode?: 'leave' | 'endzone' | 'trigger'
    reset_sec?: number
    end_zone?: string
    total_min_sec?: number
    total_max_sec?: number
  }
  const isEndZone = d.end_mode === 'endzone'
  const isTrigger = d.end_mode === 'trigger'
  const modeLabel = isEndZone ? '终点区域' : isTrigger ? '外部触发' : '离场超时'
  const detail = isEndZone
    ? `终点: ${d.end_zone || '未选'} · 兜底 ${d.reset_sec ?? 5}s`
    : isTrigger
    ? `信号触发 · 兜底 ${d.reset_sec ?? 5}s`
    : `超时 ${d.reset_sec ?? 5}s`

  const tMin = Number(d.total_min_sec ?? 0)
  const tMax = Number(d.total_max_sec ?? 0)
  const totalLabel =
    tMin > 0 && tMax > 0 ? `总耗时 ${tMin}~${tMax}s` :
    tMax > 0             ? `总耗时 ≤${tMax}s` :
    tMin > 0             ? `总耗时 ≥${tMin}s` :
                           '总耗时 不限'
  const totalOn = tMin > 0 || tMax > 0

  return (
    <div className={`rf-node rf-node-compact${selected ? ' selected' : ''}`} style={{ minWidth: 180 }}>
      <Handle type="target" position={Position.Left} id="in" />

      <div className="rf-node-header header-sopend">
        <span>🏁</span><span>结束判定</span>
      </div>
      <div className="rf-node-summary" title={modeLabel}>
        <span style={{ color: isEndZone ? '#a7f3d0' : isTrigger ? '#93c5fd' : '#fde68a' }}>{modeLabel}</span>
      </div>
      <div className="rf-node-summary" style={{ paddingTop: 0, color: '#94a3b8' }} title={detail}>
        {detail}
      </div>
      <div className="rf-node-summary" style={{ paddingTop: 0, color: totalOn ? '#67e8f9' : '#6b7280' }} title={totalLabel}>
        {totalLabel}
      </div>

      {/* 没有 source handle: 结束判定永远是链尾 */}
    </div>
  )
}
