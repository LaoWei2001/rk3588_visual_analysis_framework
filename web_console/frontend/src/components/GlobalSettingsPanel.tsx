import { useState, useEffect, useLayoutEffect, useRef } from 'react'
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
        aria-label={label} type="number" step={step} min={min} max={max}
        value={text}
        onChange={e => { setText(e.target.value); if (e.target.value !== '') onChange(Number(e.target.value)) }}
        onBlur={() => { if (text === '' || isNaN(Number(text))) setText(value != null ? String(value) : String(def)) }}
      />
    </div>
  )
}

export interface GlobalSettingsData {
  enable_display: number
  disp_width: number
  disp_height: number
  tile_rows: number
  tile_cols: number
  max_fps: number
  queue_size: number
  channel_threads: number
  tracker_enable: number
  tracker_type: string
  tracker_iou_thresh: number
  tracker_max_miss: number
  tracker_min_hits: number
  bytetrack_low_thresh: number
  bytetrack_low_iou_thresh: number
  performance_display: number
  enable_rtsp: number
  [key: string]: unknown
}

// Loading an existing file must use the same missing-field defaults as AppConfig.
export const NATIVE_GLOBAL_DEFAULTS: GlobalSettingsData = {
  enable_display: 1, disp_width: 1920, disp_height: 1080, tile_rows: 2, tile_cols: 2,
  max_fps: 30, queue_size: 1, channel_threads: 1,
  tracker_enable: 1, tracker_type: 'sort', tracker_iou_thresh: 0.3,
  tracker_max_miss: 10, tracker_min_hits: 3,
  bytetrack_low_thresh: 0.1, bytetrack_low_iou_thresh: 0.2,
  performance_display: 1, enable_rtsp: 0,
  rtsp_port: 8554, rtsp_path: '/live', rtsp_bitrate: 4096,
}

// Explicit presets for a newly created canvas; never used to fill a loaded file.
export const DEFAULT_GLOBAL_SETTINGS: GlobalSettingsData = {
  ...NATIVE_GLOBAL_DEFAULTS,
  enable_display: 0, disp_width: 640, disp_height: 640, tile_rows: 1, tile_cols: 1,
  max_fps: 25, channel_threads: 3, tracker_max_miss: 30,
  performance_display: 0, enable_rtsp: 1,
}

interface Props {
  settings: GlobalSettingsData
  onChange: (s: GlobalSettingsData) => void
  onContentHeightChange?: (height: number) => void
}

export default function GlobalSettingsPanel({ settings: s, onChange, onContentHeightChange }: Props) {
  const [collapsed, setCollapsed] = useState(false)
  const contentRef = useRef<HTMLDivElement>(null)
  useLayoutEffect(() => {
    const content = contentRef.current
    if (!content || !onContentHeightChange) return
    const measure = () => onContentHeightChange(Math.ceil(content.getBoundingClientRect().height) + 1)
    const observer = new ResizeObserver(measure)
    observer.observe(content)
    measure()
    return () => observer.disconnect()
  }, [onContentHeightChange])

  const set = (key: string, value: unknown) =>
    onChange({ ...s, [key]: value })

  return (
    <div className="gs-panel">
      <div className="gs-content" ref={contentRef}>
      <div className="gs-header" onClick={() => setCollapsed(v => !v)}>
        <span>⬡ 全局配置</span>
        <span className="gs-collapse-icon">{collapsed ? '▲' : '▼'}</span>
      </div>

      {!collapsed && (
        <div className="gs-body">
          <div className="gs-row">
            <NumField label="显示宽度"     value={s.disp_width}      def={640} onChange={v => set('disp_width', v)} />
            <NumField label="显示高度"     value={s.disp_height}     def={640} onChange={v => set('disp_height', v)} />
            <NumField label="显示窗口行数" value={s.tile_rows}       def={1}   onChange={v => set('tile_rows', v)} />
            <NumField label="显示窗口列数" value={s.tile_cols}       def={1}   onChange={v => set('tile_cols', v)} />
            <NumField label="全局处理 FPS" value={s.max_fps}         def={25}  onChange={v => set('max_fps', v)} />
            <NumField label="队列深度"     value={s.queue_size}      def={1}    onChange={v => set('queue_size', v)} />
            <NumField label="通道线程"     value={s.channel_threads} def={3}    onChange={v => set('channel_threads', v)} />
          </div>

          <div className="gs-row">
            <div className="gs-field">
              <label>跟踪算法</label>
              <select value={String(s.tracker_type ?? 'sort')} onChange={e => set('tracker_type', e.target.value)}>
                <option value="sort">SORT</option>
                <option value="bytetrack">ByteTrack</option>
              </select>
            </div>
            <NumField label={s.tracker_type === 'bytetrack' ? '高分关联 IOU' : 'SORT 匹配 IOU'} value={s.tracker_iou_thresh} def={0.3} step="0.05" min="0" max="1" onChange={v => set('tracker_iou_thresh', v)} />
            <NumField label="最大丢失帧" value={s.tracker_max_miss}   def={30}   onChange={v => set('tracker_max_miss', v)} />
            <NumField label="最小命中"   value={s.tracker_min_hits}   def={3}    onChange={v => set('tracker_min_hits', v)} />
            {s.tracker_type === 'bytetrack' && <>
              <NumField label="低分阈值" value={s.bytetrack_low_thresh} def={0.1} step="0.05" min="0" max="1" onChange={v => set('bytetrack_low_thresh', v)} />
              <NumField label="低分 IOU" value={s.bytetrack_low_iou_thresh} def={0.2} step="0.05" min="0" max="1" onChange={v => set('bytetrack_low_iou_thresh', v)} />
            </>}
          </div>

          <div className="gs-row">
            <NumField label="RTSP 端口" value={s.rtsp_port} def={8554} min="1" max="65535" onChange={v => set('rtsp_port', v)} />
            <label className="gs-field">RTSP 路径
              <input value={String(s.rtsp_path ?? '/live')} onChange={e => set('rtsp_path', e.target.value)} />
            </label>
            <NumField label="RTSP 软件编码码率 (kbps)" value={s.rtsp_bitrate} def={4096} min="1" onChange={v => set('rtsp_bitrate', v)} />
          </div>
          <div className="gs-row gs-toggles">
            <label className="gs-toggle">
              <input type="checkbox" checked={!!s.tracker_enable} onChange={e => set('tracker_enable', e.target.checked ? 1 : 0)} />
              启用跟踪器
            </label>
            <label className="gs-toggle" title="显示性能统计并输出调试日志，保存配置后生效。">
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
    </div>
  )
}
