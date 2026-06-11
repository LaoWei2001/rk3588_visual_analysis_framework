import { useState, useEffect } from 'react'
import './GlobalSettingsPanel.css'

// 数字输入框：允许整个清空后重填，不会因为删空而跳回 0；支持小数自由输入（如 "0." 不被改写）。
// 输入框用本地字符串状态；只把"有效数字"回报父级，父级值永远是合法数字。
function NumField({ label, value, def, onChange, step, min, max }: {
  label: string
  value: unknown
  def: number
  onChange: (v: number) => void
  step?: string; min?: string; max?: string
}) {
  const [text, setText] = useState(value == null ? '' : String(value))
  useEffect(() => {
    // 仅当外部值真正变化(如加载配置)时同步；用户输入造成的同值变化不回写，避免把 "0." 改成 "0"
    if (Number(text) !== Number(value)) setText(value == null ? '' : String(value))
  }, [value]) // eslint-disable-line react-hooks/exhaustive-deps
  return (
    <div className="gs-field">
      <label>{label}</label>
      <input
        type="number" step={step} min={min} max={max}
        value={text}
        onChange={e => { setText(e.target.value); if (e.target.value !== '') onChange(Number(e.target.value)) }}
        onBlur={() => { if (text === '' || isNaN(Number(text))) setText(value != null ? String(value) : String(def)) }}
      />
    </div>
  )
}

export interface GlobalSettingsData {
  model_path: string
  label_path: string
  enable_display: number
  disp_width: number
  disp_height: number
  tile_rows: number
  tile_cols: number
  max_fps: number
  queue_size: number
  channel_threads: number
  npu_cores: number
  obj_thresh: number
  nms_thresh: number
  detect_classes: string[]
  tracker_enable: number
  tracker_iou_thresh: number
  tracker_max_miss: number
  tracker_min_hits: number
  performance_display: number
  enable_pause_key: number
  enable_rtsp: number
  [key: string]: unknown
}

export const DEFAULT_GLOBAL_SETTINGS: GlobalSettingsData = {
  model_path: 'assets/yolov8n.rknn',
  label_path: 'assets/labels.txt',
  enable_display: 0,
  disp_width: 1920, disp_height: 1080,
  tile_rows: 2, tile_cols: 2,
  max_fps: 15, queue_size: 1,
  channel_threads: 1, npu_cores: 3,
  obj_thresh: 0.3, nms_thresh: 0.45,
  detect_classes: [],
  tracker_enable: 1, tracker_iou_thresh: 0.3,
  tracker_max_miss: 30, tracker_min_hits: 3,
  performance_display: 1, enable_pause_key: 0,
  enable_rtsp: 0,
}

interface Props {
  settings: GlobalSettingsData
  onChange: (s: GlobalSettingsData) => void
}

export default function GlobalSettingsPanel({ settings: s, onChange }: Props) {
  const [collapsed, setCollapsed] = useState(false)

  const set = (key: string, value: unknown) =>
    onChange({ ...s, [key]: value })

  return (
    <div className="gs-panel">
      <div className="gs-header" onClick={() => setCollapsed(v => !v)}>
        <span>⬡ 全局配置</span>
        <span className="gs-collapse-icon">{collapsed ? '▲' : '▼'}</span>
      </div>

      {!collapsed && (
        <div className="gs-body">
          <div className="gs-row">
            <div className="gs-field gs-wide">
              <label>全局模型路径</label>
              <input value={String(s.model_path ?? '')} onChange={e => set('model_path', e.target.value)} />
            </div>
            <div className="gs-field gs-wide">
              <label>标签文件路径</label>
              <input value={String(s.label_path ?? '')} onChange={e => set('label_path', e.target.value)} />
            </div>
          </div>

          <div className="gs-row">
            <NumField label="显示宽度"     value={s.disp_width}      def={1920} onChange={v => set('disp_width', v)} />
            <NumField label="显示高度"     value={s.disp_height}     def={1080} onChange={v => set('disp_height', v)} />
            <NumField label="显示窗口行数" value={s.tile_rows}       def={2}    onChange={v => set('tile_rows', v)} />
            <NumField label="显示窗口列数" value={s.tile_cols}       def={2}    onChange={v => set('tile_cols', v)} />
            <NumField label="最大 FPS"     value={s.max_fps}         def={15}   onChange={v => set('max_fps', v)} />
            <NumField label="队列深度"     value={s.queue_size}      def={1}    onChange={v => set('queue_size', v)} />
            <NumField label="通道线程"     value={s.channel_threads} def={1}    onChange={v => set('channel_threads', v)} />
            <div className="gs-field">
              <label>NPU 核心数</label>
              <select value={Number(s.npu_cores ?? 3)} onChange={e => set('npu_cores', +e.target.value)}>
                {[1, 2, 3].map(v => <option key={v} value={v}>{v}</option>)}
              </select>
            </div>
          </div>

          <div className="gs-row">
            <NumField label="置信阈值"   value={s.obj_thresh}         def={0.3}  step="0.05" min="0" max="1" onChange={v => set('obj_thresh', v)} />
            <NumField label="NMS 阈值"   value={s.nms_thresh}         def={0.45} step="0.05" min="0" max="1" onChange={v => set('nms_thresh', v)} />
            <NumField label="Tracker IOU" value={s.tracker_iou_thresh} def={0.3} step="0.05" min="0" max="1" onChange={v => set('tracker_iou_thresh', v)} />
            <NumField label="最大丢失帧" value={s.tracker_max_miss}   def={30}   onChange={v => set('tracker_max_miss', v)} />
            <NumField label="最小命中"   value={s.tracker_min_hits}   def={3}    onChange={v => set('tracker_min_hits', v)} />
          </div>

          <div className="gs-row gs-toggles">
            <label className="gs-toggle">
              <input type="checkbox" checked={!!s.tracker_enable} onChange={e => set('tracker_enable', e.target.checked ? 1 : 0)} />
              启用跟踪器
            </label>
            <label className="gs-toggle">
              <input type="checkbox" checked={!!s.performance_display} onChange={e => set('performance_display', e.target.checked ? 1 : 0)} />
              性能显示
            </label>
            <label className="gs-toggle">
              <input type="checkbox" checked={!!s.enable_display} onChange={e => set('enable_display', e.target.checked ? 1 : 0)} />
              HDMI 显示
            </label>
            <label className="gs-toggle">
              <input type="checkbox" checked={!!s.enable_rtsp} onChange={e => set('enable_rtsp', e.target.checked ? 1 : 0)} />
              RTSP 推流
            </label>
          </div>
        </div>
      )}
    </div>
  )
}
