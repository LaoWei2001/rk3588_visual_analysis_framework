import { Handle, Position, NodeProps, useReactFlow } from '@xyflow/react'
import './nodeStyles.css'

// 一个步骤节点 = SOP 流程里的一站。仅用于「SOP 流程配置」弹窗内部的子画布。
// 自包含: 全部状态来自 node.data, 不依赖任何全局 store。
//   data: { zoneName, enter_sec, dwell_min_sec, seq? }  (seq 由弹窗按连线顺序注入, 仅用于显示序号)
//   - 左把手 in / 右把手 out: 连成一条链 = 期望经过顺序。
//   - 同一区域可被多个步骤引用(多次进入), 每步参数各自独立。
export default function SopStepNode({ id, data, selected }: NodeProps) {
  const d = data as { zoneName?: string; enter_sec?: number; dwell_min_sec?: number; seq?: number }
  const { deleteElements } = useReactFlow()

  const name  = d.zoneName?.trim() || '未选区域'
  const dwell = d.dwell_min_sec ?? 0

  return (
    <div className={`rf-node rf-node-compact${selected ? ' selected' : ''}`} style={{ minWidth: 156 }}>
      <Handle type="target" position={Position.Left} id="in" />

      <div className="rf-node-header header-sop">
        <span className="sop-step-idx">{d.seq && d.seq > 0 ? d.seq : '?'}</span>
        <span>步骤</span>
        <button className="sop-step-del" title="删除该步骤" onClick={() => deleteElements({ nodes: [{ id }] })}>✕</button>
      </div>

      <div className="rf-node-summary" title={name}>
        <span className={`sop-step-name${d.zoneName ? '' : ' unset'}`}>{name}</span>
      </div>

      <div className="sop-step-foot">
        <span className="sop-step-param" title="进入确认时长">进入 {d.enter_sec ?? 0}s</span>
        <span className={`sop-step-param${dwell > 0 ? ' on' : ''}`} title="该步骤要求的最小停留">
          {dwell > 0 ? `停留≥${dwell}s` : '停留不限'}
        </span>
      </div>

      <Handle type="source" position={Position.Right} id="out" />
    </div>
  )
}
