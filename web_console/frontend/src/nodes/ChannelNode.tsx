import { NodeProps, useReactFlow } from '@xyflow/react'
import { useState } from 'react'
import { useConsoleStore } from '../store/consoleStore'
import { useROIStore } from '../store/roiStore'
import ROIModal from '../components/ROIModal'
import NumberField from '../components/NumberField'
import './nodeStyles.css'

const EMPTY_POLYGON: number[][] = []

const SRC_TYPES = [
  { value: 'rtsp', label: 'RTSP 摄像头' },
  { value: 'usb',  label: 'USB 摄像头'  },
  { value: 'file', label: '本地文件'    },
]
const DIFY_LOGICS   = ['logic_dify', 'logic_dify_person_verify']
const RADIUS_LOGICS = ['logic_roll', 'logic_hook']

export default function ChannelNode({ id, data, selected }: NodeProps) {
  const { updateNodeData } = useReactFlow()
  const info    = useConsoleStore(s => s.info)
  // 兼容字段: 此节点为旧版整合节点(未在画布启用), 仅取第一个区域用于显示
  const polygon = useROIStore(s => s.zones[id]?.[0]?.polygon ?? EMPTY_POLYGON)
  const [showROI,       setShowROI      ] = useState(false)
  const [streamOpen,    setStreamOpen   ] = useState(true)
  const [inferenceOpen, setInferenceOpen] = useState(true)

  const d   = data as Record<string, unknown>
  const set = (key: string, val: unknown) => updateNodeData(id, { [key]: val })

  const srcType     = (d.src_type as string) ?? 'rtsp'
  const logic       = String(d.logic ?? 'logic_default')
  const logics      = info?.known_channel_logics ?? []
  const modelTypes  = info?.known_model_types    ?? []
  const hasROI      = polygon.length > 0

  const setSrcType = (type: string) => {
    const patch: Record<string, unknown> = { src_type: type }
    if (type === 'rtsp') { patch.url = d.url ?? ''; patch.video_enc = d.video_enc ?? 'h264' }
    if (type === 'usb')  { patch.device = d.device ?? '/dev/video0' }
    if (type === 'file') { patch.url = d.url ?? ''; patch.loop = d.loop ?? true }
    updateNodeData(id, patch)
  }

  return (
    <>
      <div className={`rf-node${selected ? ' selected' : ''}`} style={{ minWidth: 300 }}>

        {/* ─── Header ─── */}
        <div className="rf-node-header header-channel">
          <span>◉</span>
          <span>通道{d.id !== undefined ? ` #${d.id}` : ''}</span>
          <label className="node-toggle" style={{ marginLeft: 'auto' }}>
            <input
              type="checkbox"
              checked={d.enable !== false}
              onChange={e => set('enable', e.target.checked)}
            />
            启用
          </label>
        </div>

        <div className="rf-node-body">

          {/* ─── Video source section ─── */}
          <div className="node-section-hdr" onClick={() => setStreamOpen(v => !v)}>
            <span>📹 视频源</span>
            <span className="node-section-arrow">{streamOpen ? '▲' : '▼'}</span>
          </div>

          {streamOpen && (
            <div className="node-section-body">
              <div className="node-field">
                <label>输入类型</label>
                <select value={srcType} onChange={e => setSrcType(e.target.value)}>
                  {SRC_TYPES.map(t => <option key={t.value} value={t.value}>{t.label}</option>)}
                </select>
              </div>

              {srcType === 'rtsp' && <>
                <div className="node-field">
                  <label>RTSP 地址</label>
                  <input
                    value={String(d.url ?? '')}
                    onChange={e => set('url', e.target.value)}
                    placeholder="rtsp://user:pass@192.168.1.1/stream"
                  />
                </div>
                <div className="node-field">
                  <label>编码格式</label>
                  <select value={String(d.video_enc ?? 'h264')} onChange={e => set('video_enc', e.target.value)}>
                    <option value="h264">H.264</option>
                    <option value="h265">H.265</option>
                  </select>
                </div>
              </>}

              {srcType === 'usb' && (
                <div className="node-field">
                  <label>设备路径</label>
                  <input
                    value={String(d.device ?? '/dev/video0')}
                    onChange={e => set('device', e.target.value)}
                    placeholder="/dev/video0"
                  />
                </div>
              )}

              {srcType === 'file' && <>
                <div className="node-field">
                  <label>文件路径</label>
                  <input
                    value={String(d.url ?? '')}
                    onChange={e => set('url', e.target.value)}
                    placeholder="assets/video.mp4"
                  />
                </div>
                <label className="node-toggle">
                  <input type="checkbox" checked={!!d.loop} onChange={e => set('loop', e.target.checked)} />
                  循环播放
                </label>
              </>}
            </div>
          )}

          {/* ─── Inference section ─── */}
          <div className="node-section-hdr" onClick={() => setInferenceOpen(v => !v)}>
            <span>🧠 推理配置</span>
            <span className="node-section-arrow">{inferenceOpen ? '▲' : '▼'}</span>
          </div>

          {inferenceOpen && (
            <div className="node-section-body">
              <div className="node-row">
                <div className="node-field">
                  <label>NPU 核心</label>
                  <select value={String(d.npu_core ?? 0)} onChange={e => set('npu_core', +e.target.value)}>
                    <option value="0">核心 0</option>
                    <option value="1">核心 1</option>
                    <option value="2">核心 2</option>
                  </select>
                </div>
                <div className="node-field" style={{ flex: 2 }}>
                  <label>算法逻辑</label>
                  <select value={logic} onChange={e => set('logic', e.target.value)}>
                    {logics.map(l => <option key={l} value={l}>{l}</option>)}
                    {!logics.includes(logic) && <option value={logic}>{logic}</option>}
                  </select>
                </div>
              </div>

              <div className="node-field">
                <label>模型类型</label>
                <select value={String(d.model_type ?? 'yolov8_det')} onChange={e => set('model_type', e.target.value)}>
                  {modelTypes.map(t => <option key={t} value={t}>{t}</option>)}
                </select>
              </div>

              <div className="node-field">
                <label>模型路径 (.rknn)</label>
                <input
                  value={String(d.model_path ?? '')}
                  onChange={e => set('model_path', e.target.value)}
                  placeholder="assets/model.rknn"
                />
              </div>

              <div className="node-field">
                <label>标签文件 (.txt)</label>
                <input
                  value={String(d.label_path ?? '')}
                  onChange={e => set('label_path', e.target.value)}
                  placeholder="assets/labels.txt"
                />
              </div>

              <div className="node-row">
                <div className="node-field">
                  <label>置信阈值</label>
                  <NumberField
                    step="0.05" min="0" max="1" def={0.3}
                    value={d.obj_thresh}
                    onChange={v => set('obj_thresh', v ?? 0.3)}
                  />
                </div>
                <div className="node-field">
                  <label>NMS 阈值</label>
                  <NumberField
                    step="0.05" min="0" max="1" def={0.45}
                    value={d.nms_thresh}
                    onChange={v => set('nms_thresh', v ?? 0.45)}
                  />
                </div>
              </div>

              <div className="node-field">
                <label>检测类别（空 = 全部）</label>
                <input
                  value={((d.detect_classes as string[]) ?? []).join(', ')}
                  onChange={e =>
                    set('detect_classes', e.target.value.split(',').map(s => s.trim()).filter(Boolean))
                  }
                  placeholder="person, car, ..."
                />
              </div>

              {DIFY_LOGICS.includes(logic) && (
                <div className="node-field">
                  <label>Dify 提示词</label>
                  <textarea
                    rows={3}
                    value={String(d.dify_prompt ?? '')}
                    onChange={e => set('dify_prompt', e.target.value)}
                    style={{ resize: 'vertical' }}
                  />
                </div>
              )}

              {RADIUS_LOGICS.includes(logic) && (
                <div className="node-field">
                  <label>radius 参数</label>
                  <NumberField
                    def={50}
                    value={d.radius}
                    onChange={v => set('radius', v ?? 50)}
                  />
                </div>
              )}
            </div>
          )}

          {/* ─── ROI ─── */}
          <div className="node-roi-row">
            <button
              className={`node-btn node-roi-btn${hasROI ? ' primary' : ''}`}
              onClick={() => setShowROI(true)}
            >
              {hasROI ? `✔ ROI 已配置（${polygon.length} 个顶点）` : '＋ 配置 ROI 检测区域'}
            </button>
          </div>
        </div>
      </div>

      {showROI && <ROIModal nodeId={id} onClose={() => setShowROI(false)} />}
    </>
  )
}
