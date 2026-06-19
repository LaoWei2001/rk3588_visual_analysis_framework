import { Handle, Position, NodeProps, useReactFlow } from '@xyflow/react'
import { useState } from 'react'
import NumberField from '../components/NumberField'
import './nodeStyles.css'

export default function GlobalNode({ id, data, selected }: NodeProps) {
  const { updateNodeData } = useReactFlow()
  const d = data as Record<string, unknown>
  const [collapsed, setCollapsed] = useState(false)

  const set = (key: string, value: unknown) => updateNodeData(id, { [key]: value })

  return (
    <div className={`rf-node${selected ? ' selected' : ''}`} style={{ minWidth: 300 }}>
      <div className="rf-node-header header-global">
        <span>⬡</span>
        <span>全局配置</span>
        <span style={{ marginLeft: 'auto', cursor: 'pointer', opacity: 0.7 }} onClick={() => setCollapsed(v => !v)}>
          {collapsed ? '▼' : '▲'}
        </span>
      </div>

      {!collapsed && (
        <div className="rf-node-body">
          <div className="node-row">
            <div className="node-field">
              <label>全局模型路径</label>
              <input value={String(d.model_path ?? '')} onChange={e => set('model_path', e.target.value)} />
            </div>
          </div>
          <div className="node-field">
            <label>标签文件路径</label>
            <input value={String(d.label_path ?? '')} onChange={e => set('label_path', e.target.value)} />
          </div>

          <hr className="node-divider" />

          <div className="node-row">
            <div className="node-field">
              <label>显示宽度</label>
              <NumberField def={1920} value={d.disp_width} onChange={v => set('disp_width', v ?? 1920)} />
            </div>
            <div className="node-field">
              <label>显示高度</label>
              <NumberField def={1080} value={d.disp_height} onChange={v => set('disp_height', v ?? 1080)} />
            </div>
          </div>
          <div className="node-row">
            <div className="node-field">
              <label>瓦片行数</label>
              <NumberField def={2} value={d.tile_rows} onChange={v => set('tile_rows', v ?? 2)} />
            </div>
            <div className="node-field">
              <label>瓦片列数</label>
              <NumberField def={2} value={d.tile_cols} onChange={v => set('tile_cols', v ?? 2)} />
            </div>
          </div>

          <hr className="node-divider" />

          <div className="node-row">
            <div className="node-field">
              <label>最大 FPS</label>
              <NumberField def={15} value={d.max_fps} onChange={v => set('max_fps', v ?? 15)} />
            </div>
            <div className="node-field">
              <label>队列深度</label>
              <NumberField def={1} value={d.queue_size} onChange={v => set('queue_size', v ?? 1)} />
            </div>
            <div className="node-field">
              <label>NPU 核心数</label>
              <select value={String(d.npu_cores ?? 3)} onChange={e => set('npu_cores', +e.target.value)}>
                {[1, 2, 3].map(v => <option key={v} value={v}>{v}</option>)}
              </select>
            </div>
          </div>

          <div className="node-row">
            <div className="node-field">
              <label>置信阈值</label>
              <NumberField step="0.05" min="0" max="1" def={0.3} value={d.obj_thresh} onChange={v => set('obj_thresh', v ?? 0.3)} />
            </div>
            <div className="node-field">
              <label>NMS 阈值</label>
              <NumberField step="0.05" min="0" max="1" def={0.45} value={d.nms_thresh} onChange={v => set('nms_thresh', v ?? 0.45)} />
            </div>
          </div>

          <hr className="node-divider" />

          <div className="node-row">
            <label className="node-toggle">
              <input type="checkbox" checked={!!d.tracker_enable} onChange={e => set('tracker_enable', e.target.checked ? 1 : 0)} />
              启用跟踪器
            </label>
            <label className="node-toggle">
              <input type="checkbox" checked={!!d.performance_display} onChange={e => set('performance_display', e.target.checked ? 1 : 0)} />
              性能显示
            </label>
            <label className="node-toggle">
              <input type="checkbox" checked={!!d.enable_pause_key} onChange={e => set('enable_pause_key', e.target.checked ? 1 : 0)} />
              暂停键
            </label>
          </div>
        </div>
      )}

      {collapsed && (
        <div className="rf-node-body">
          <span className="collapsed-badge">
            {String(d.disp_width ?? 1920)}×{String(d.disp_height ?? 1080)} · NPU×{String(d.npu_cores ?? 3)} · {String(d.max_fps ?? 15)}fps
          </span>
        </div>
      )}

      <Handle type="source" position={Position.Bottom} id="out" />
    </div>
  )
}
