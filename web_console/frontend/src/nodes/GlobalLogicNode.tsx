import { Handle, Position, NodeProps, useReactFlow } from '@xyflow/react'
import { useConsoleStore } from '../store/consoleStore'
import NumberField from '../components/NumberField'
import './nodeStyles.css'

export default function GlobalLogicNode({ id, data, selected }: NodeProps) {
  const { updateNodeData } = useReactFlow()
  const info = useConsoleStore(s => s.info)
  const d = data as Record<string, unknown>
  const set = (key: string, val: unknown) => updateNodeData(id, { [key]: val })

  const globalLogics = info?.known_global_logics ?? ['global_example', 'global_default']
  const currentLogic = String(d.logic ?? 'global_default')

  const channelsRaw = (d.channels as number[]) ?? []
  const channelsStr = channelsRaw.join(', ')

  return (
    <div className={`rf-node${selected ? ' selected' : ''}`} style={{ minWidth: 240 }}>
      <Handle type="target" position={Position.Top} id="in" />

      <div className="rf-node-header header-logic">
        <span>⬡</span>
        <span>全局逻辑</span>
        <label className="node-toggle" style={{ marginLeft: 'auto' }}>
          <input type="checkbox" checked={d.enable !== false} onChange={e => set('enable', e.target.checked)} />
          启用
        </label>
      </div>

      <div className="rf-node-body">
        <div className="node-field">
          <label>逻辑名称</label>
          <select value={currentLogic} onChange={e => set('logic', e.target.value)}>
            {globalLogics.map(l => <option key={l} value={l}>{l}</option>)}
            {!globalLogics.includes(currentLogic) && <option value={currentLogic}>{currentLogic}</option>}
          </select>
        </div>

        <div className="node-field">
          <label>监控通道（空=全部，逗号分隔 ID）</label>
          <input
            value={channelsStr}
            onChange={e => set('channels', e.target.value.split(',').map(s => s.trim()).filter(Boolean).map(Number).filter(n => !isNaN(n)))}
            placeholder="0, 1, 2"
          />
        </div>

        <div className="node-field">
          <label>轮询间隔 (ms)</label>
          <NumberField def={200} value={d.poll_interval_ms} onChange={v => set('poll_interval_ms', v ?? 200)} />
        </div>
      </div>
    </div>
  )
}
