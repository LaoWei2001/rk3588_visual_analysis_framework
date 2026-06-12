/**
 * NodeConfigPanel — 右侧侧边栏，单击节点后显示该节点的完整配置表单。
 * 表单数据通过 onUpdate(nodeId, patch) 回调写回父组件的 nodes 状态。
 */

import { useState, useEffect, type ReactNode } from 'react'
import { Node } from '@xyflow/react'
import { useEditorStore }  from '../store/editorStore'
import { useConsoleStore } from '../store/consoleStore'
import { useROIStore, type Zone } from '../store/roiStore'
import { fetchAppLogics, asLogicDef, uploadAsset, deleteAsset, type LogicDef, type LogicParam } from '../api/client'
import { getSrcType, SRC_TYPES } from '../utils/streamSource'
import AssetPicker         from './AssetPicker'
import NumberField         from './NumberField'
import './NodeConfigPanel.css'

// Stable empty-array constant — MUST NOT be an inline `[]` literal inside a Zustand selector,
// because `useSyncExternalStore` compares snapshots with Object.is on every render.
// An inline `[]` returns a new reference each time, causing an infinite re-render loop.
const EMPTY_ZONES: Zone[] = []

// 切到 RTSP 时的默认地址
const DEFAULT_RTSP_URL = 'rtsp://admin:jndxc301@192.168.2.150/Streaming/Channels/101'

interface Props {
  node: Node | null
  onUpdate: (nodeId: string, patch: Record<string, unknown>) => void
}

// ── Header label per type ────────────────────────────────────────────────────
const NODE_TITLES: Record<string, [string, string]> = {
  stream: ['◈', '视频流节点'],
  model:  ['🧠', 'YOLO推理节点'],
  roi:    ['◆', 'ROI区域节点'],
  logic:  ['⚡', '逻辑函数节点'],
  report: ['📡', '上报配置节点'],
}

const HEADER_CLASS: Record<string, string> = {
  stream: 'header-stream',
  model:  'header-model',
  roi:    'header-roi',
  logic:  'header-logic',
  report: 'header-report',
}

export default function NodeConfigPanel({ node, onUpdate }: Props) {
  if (!node) {
    return (
      <div className="ncp ncp-empty">
        <div className="ncp-empty-icon">☰</div>
        <div className="ncp-empty-text">单击画布中的节点<br />可在此处查看 / 编辑配置</div>
      </div>
    )
  }

  const [icon, title] = NODE_TITLES[node.type ?? ''] ?? ['⬡', '节点']
  const headerCls = HEADER_CLASS[node.type ?? ''] ?? ''

  return (
    <div className="ncp">
      <div className={`ncp-header ${headerCls}`}>
        <span>{icon}</span>
        <span>{title}</span>
        <span className="ncp-node-id">#{node.id.split('-')[1]}</span>
      </div>

      <div className="ncp-body">
        {node.type === 'stream' && <StreamForm  node={node} onUpdate={onUpdate} />}
        {node.type === 'model'  && <ModelForm   node={node} onUpdate={onUpdate} />}
        {node.type === 'logic'  && <LogicForm   node={node} onUpdate={onUpdate} />}
        {node.type === 'report' && <ReportForm  node={node} onUpdate={onUpdate} />}
        {node.type === 'roi'    && <ROIInfo     node={node} />}
      </div>
    </div>
  )
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: generic field wrapper
// ─────────────────────────────────────────────────────────────────────────────
function F({ label, children }: { label: string; children: ReactNode }) {
  return (
    <div className="node-field">
      <label>{label}</label>
      {children}
    </div>
  )
}

// ─────────────────────────────────────────────────────────────────────────────
// 资源导入：重名提示覆盖，不覆盖则后端另存为 _copy；成功后刷新列表并自动选中。
// ─────────────────────────────────────────────────────────────────────────────
function useAssetUpload() {
  const appName    = useEditorStore(s => s.appName)
  const loadAssets = useEditorStore(s => s.loadAssets)
  const [busy, setBusy]         = useState<string | null>(null)   // 正在上传的字段名
  const [progress, setProgress] = useState(0)                     // 该字段的上传进度 0–100

  const upload = async (
    field: string,
    file: File,
    existing: string[],                 // 同类已有文件（用于重名判断）
    onDone: (path: string) => void,     // 写回节点字段（自动选中）
  ) => {
    if (!appName) { window.alert('未选择程序，无法导入'); return }
    const dup = existing.some(p => (p.split('/').pop() ?? p) === file.name)
    let overwrite = false
    if (dup) {
      overwrite = window.confirm(
        `assets/ 下已存在同名文件「${file.name}」。\n\n` +
        `确定 = 覆盖原文件\n取消 = 保留原文件，另存为副本（文件名加 _copy）`,
      )
    }
    setBusy(field)
    setProgress(0)
    try {
      const r = await uploadAsset(appName, file, overwrite, pct => setProgress(pct))
      await loadAssets(appName)         // 刷新下拉列表
      onDone(r.path)                    // 自动选中刚导入的文件
      if (r.renamed) window.alert(`原文件已保留，新文件另存为 ${r.name}`)
    } catch (e: unknown) {
      const msg = (e as { response?: { data?: { detail?: string } } })?.response?.data?.detail
        ?? (e instanceof Error ? e.message : String(e))
      window.alert(`导入失败：${msg}`)
    } finally {
      setBusy(null)
      setProgress(0)
    }
  }

  return { busy, progress, upload }
}

// ─────────────────────────────────────────────────────────────────────────────
// 资源删除：确认后删除远端 RK3588 上的文件，刷新列表，若删除的是当前选中项则清空。
// ─────────────────────────────────────────────────────────────────────────────
function useAssetDelete(currentValue: string, onChange: (v: string) => void) {
  const appName    = useEditorStore(s => s.appName)
  const loadAssets = useEditorStore(s => s.loadAssets)

  const del = async (path: string) => {
    if (!appName) throw new Error('未选择程序')
    await deleteAsset(appName, path)
    await loadAssets(appName)
    if (path === currentValue) onChange('')
  }

  return del
}

// ─────────────────────────────────────────────────────────────────────────────
// Stream form
// ─────────────────────────────────────────────────────────────────────────────
function StreamForm({ node, onUpdate }: { node: Node; onUpdate: Props['onUpdate'] }) {
  const assets  = useEditorStore(s => s.assets)
  const { busy, progress, upload } = useAssetUpload()
  const d   = node.data as Record<string, unknown>
  const set = (k: string, v: unknown) => onUpdate(node.id, { [k]: v })

  const deleteAsset_ = useAssetDelete(String(d.url ?? ''), v => set('url', v))
  const srcType = getSrcType(d)

  const setSrcType = (type: string) => {
    const patch: Record<string, unknown> = { src_type: type }
    const cur = String(d.url ?? '')
    if (type === 'rtsp') {
      // 从非 RTSP(如本地文件)切过来时 URL 还停留在旧地址 → 用默认 RTSP 地址
      patch.url = cur.startsWith('rtsp://') ? cur : DEFAULT_RTSP_URL
      patch.video_enc = d.video_enc ?? 'h264'
    }
    if (type === 'usb')  { patch.device = d.device ?? '/dev/video0' }
    if (type === 'file') {
      // 从 RTSP 切到文件时清掉 rtsp 地址，避免文件选择器里残留 rtsp://
      patch.url = cur.startsWith('rtsp://') ? '' : cur
      patch.loop = d.loop ?? true
    }
    onUpdate(node.id, patch)
  }

  return (
    <div className="ncp-form">
      <F label="通道编号 (channel_id)">
        <NumberField min="0" def={0} value={d.channel_id} onChange={v => set('channel_id', v ?? 0)} />
      </F>

      <F label="输入类型">
        <select value={srcType} onChange={e => setSrcType(e.target.value)}>
          {!srcType && <option value="" disabled>请选择视频源类型（src_type 必填）…</option>}
          {SRC_TYPES.map(t => <option key={t.value} value={t.value}>{t.label}</option>)}
        </select>
      </F>
      {!srcType && (
        <div style={{ color: '#f59e0b', fontSize: 12, marginTop: -6, marginBottom: 6 }}>
          ⚠ 该配置未指定视频源类型，请在上方选择（已取消按地址自动识别）。
        </div>
      )}

      {srcType === 'rtsp' && <>
        <F label="RTSP 地址">
          <input
            value={String(d.url ?? '')}
            onChange={e => set('url', e.target.value)}
            placeholder="rtsp://user:pass@192.168.1.x/stream"
          />
        </F>
        <F label="编码格式">
          <select value={String(d.video_enc ?? 'h264')} onChange={e => set('video_enc', e.target.value)}>
            <option value="h264">H.264</option>
            <option value="h265">H.265</option>
          </select>
        </F>
      </>}

      {srcType === 'usb' && <>
        <F label="设备路径">
          <input
            value={String(d.device ?? '/dev/video0')}
            onChange={e => set('device', e.target.value)}
            placeholder="/dev/video0"
          />
        </F>
        <F label="采集分辨率（与 ROI 抓帧一致，不随最大FPS变）">
          <select
            value={`${Number(d.usb_width ?? 0)}x${Number(d.usb_height ?? 0)}`}
            onChange={e => {
              const [w, h] = e.target.value.split('x').map(Number)
              onUpdate(node.id, { usb_width: w, usb_height: h })
            }}>
            <option value="0x0">自动（随最大FPS，旧行为）</option>
            <option value="1280x720">1280×720（16:9，推荐）</option>
            <option value="640x480">640×480（4:3）</option>
            <option value="1280x960">1280×960（4:3）</option>
            <option value="1920x1080">1920×1080（16:9）</option>
          </select>
        </F>
      </>}

      {srcType === 'file' && <>
        <F label="视频文件">
          <AssetPicker
            value={String(d.url ?? '')}
            onChange={v => set('url', v)}
            options={assets.videos}
            emptyHint="该程序 assets/ 下暂无视频文件，请点「导入」上传"
            accept=".mp4,.avi,.mkv"
            uploading={busy === 'url'}
            progress={busy === 'url' ? progress : 0}
            onUpload={f => upload('url', f, assets.videos, p => set('url', p))}
            onDelete={deleteAsset_}
          />
        </F>
        <label className="node-toggle">
          <input type="checkbox" checked={!!d.loop} onChange={e => set('loop', e.target.checked)} />
          循环播放
        </label>
      </>}
    </div>
  )
}

// ─────────────────────────────────────────────────────────────────────────────
// Model form
// ─────────────────────────────────────────────────────────────────────────────
function ModelForm({ node, onUpdate }: { node: Node; onUpdate: Props['onUpdate'] }) {
  const assets     = useEditorStore(s => s.assets)
  const info       = useConsoleStore(s => s.info)
  const modelTypes = info?.known_model_types ?? ['yolov8_det', 'yolov5', 'yolov8_pose', 'yolov5_seg']
  const { busy, progress, upload } = useAssetUpload()

  const d   = node.data as Record<string, unknown>
  const set = (k: string, v: unknown) => onUpdate(node.id, { [k]: v })
  const deleteModel = useAssetDelete(String(d.model_path ?? ''), v => set('model_path', v))
  const deleteLabel = useAssetDelete(String(d.label_path ?? ''), v => set('label_path', v))

  return (
    <div className="ncp-form">
      <label className="node-toggle ncp-top-toggle">
        <input type="checkbox" checked={d.infer_enable !== false} onChange={e => set('infer_enable', e.target.checked)} />
        启用推理
      </label>
      <div style={{ fontSize: 12, color: '#94a3b8', marginTop: -6, marginBottom: 6 }}>
        关闭=该通道不进 NPU 推理（视频与逻辑照常运行，适合传统算法通道）
      </div>

      <div className="node-row">
        <F label="NPU 核心">
          <select value={String(d.npu_core ?? 0)} onChange={e => set('npu_core', +e.target.value)}>
            <option value="0">核心 0</option>
            <option value="1">核心 1</option>
            <option value="2">核心 2</option>
          </select>
        </F>
        <F label="模型类型">
          <select value={String(d.model_type ?? 'yolov8_det')} onChange={e => set('model_type', e.target.value)}>
            {modelTypes.map(t => <option key={t} value={t}>{t}</option>)}
          </select>
        </F>
      </div>

      <F label="模型文件 (.rknn)">
        <AssetPicker
          value={String(d.model_path ?? '')}
          onChange={v => set('model_path', v)}
          options={assets.models}
          emptyHint="该程序 assets/ 下暂无 .rknn 模型，请点「导入」上传"
          accept=".rknn"
          uploading={busy === 'model_path'}
          progress={busy === 'model_path' ? progress : 0}
          onUpload={f => upload('model_path', f, assets.models, p => set('model_path', p))}
          onDelete={deleteModel}
        />
      </F>

      <F label="标签文件 (.txt)">
        <AssetPicker
          value={String(d.label_path ?? '')}
          onChange={v => set('label_path', v)}
          options={assets.labels}
          emptyHint="该程序 assets/ 下暂无 .txt 标签文件，请点「导入」上传"
          accept=".txt"
          uploading={busy === 'label_path'}
          progress={busy === 'label_path' ? progress : 0}
          onUpload={f => upload('label_path', f, assets.labels, p => set('label_path', p))}
          onDelete={deleteLabel}
        />
      </F>

      <div className="node-row">
        <F label="置信阈值">
          <NumberField step="0.05" min="0" max="1" def={0.3}
            value={d.obj_thresh} onChange={v => set('obj_thresh', v ?? 0.3)} />
        </F>
        <F label="NMS 阈值">
          <NumberField step="0.05" min="0" max="1" def={0.45}
            value={d.nms_thresh} onChange={v => set('nms_thresh', v ?? 0.45)} />
        </F>
      </div>

      <F label="检测类别（空 = 全部）">
        <input
          value={((d.detect_classes as string[]) ?? []).join(', ')}
          onChange={e => set('detect_classes', e.target.value.split(',').map(s => s.trim()).filter(Boolean))}
          placeholder="person, car, ..."
        />
      </F>

      <div className="ncp-divider" />

      {/* Tracker overrides */}
      <div className="ncp-section-label">Tracker 覆盖（空 = 使用全局）</div>
      <div className="node-row">
        <F label="IOU 阈值">
          <NumberField allowEmpty step="0.05" min="0" max="1" placeholder="全局"
            value={d.tracker_iou_thresh} onChange={v => set('tracker_iou_thresh', v)} />
        </F>
        <F label="最大丢失帧">
          <NumberField allowEmpty placeholder="全局"
            value={d.tracker_max_miss} onChange={v => set('tracker_max_miss', v)} />
        </F>
      </div>
      <div className="node-row">
        <F label="最小命中">
          <NumberField allowEmpty placeholder="全局"
            value={d.tracker_min_hits} onChange={v => set('tracker_min_hits', v)} />
        </F>
        <F label="线程数">
          <NumberField allowEmpty placeholder="全局"
            value={d.threads} onChange={v => set('threads', v)} />
        </F>
      </div>
    </div>
  )
}

// ─────────────────────────────────────────────────────────────────────────────
// Logic form
// ─────────────────────────────────────────────────────────────────────────────
function LogicForm({ node, onUpdate }: { node: Node; onUpdate: Props['onUpdate'] }) {
  const info    = useConsoleStore(s => s.info)
  const appName = useEditorStore(s => s.appName)
  const [defs, setDefs] = useState<LogicDef[] | null>(null)

  useEffect(() => {
    if (!appName) return
    fetchAppLogics(appName)
      .then(res => setDefs(res.channel_logics.map(asLogicDef)))
      .catch(() => setDefs(null))
  }, [appName])

  // 动态清单(logics.json) → 回退到 console 已知名字 → 内置名字
  const logicDefs: LogicDef[] = defs
    ?? (info?.known_channel_logics ?? [
        'logic_default', 'logic_server', 'logic_dify', 'logic_hook',
        'logic_roll', 'logic_custom', 'logic_person_alarm',
      ]).map(asLogicDef)

  const d      = node.data as Record<string, unknown>
  const logic  = String(d.logic ?? 'logic_default')
  const names  = logicDefs.map(x => x.name)
  const curDef = logicDefs.find(x => x.name === logic)
  const params = curDef?.params ?? []
  const set    = (k: string, v: unknown) => onUpdate(node.id, { [k]: v })

  // 切换 logic：写名字，并为新 logic 的参数补默认值（若尚无值）
  const selectLogic = (name: string) => {
    const def = logicDefs.find(x => x.name === name)
    const patch: Record<string, unknown> = { logic: name }
    ;(def?.params ?? []).forEach(p => {
      if (d[p.key] === undefined && p.default !== undefined) patch[p.key] = p.default
    })
    onUpdate(node.id, patch)
  }

  return (
    <div className="ncp-form">
      <F label="逻辑名称（下拉选择）">
        <select value={names.includes(logic) ? logic : '__custom__'}
          onChange={e => { if (e.target.value !== '__custom__') selectLogic(e.target.value) }}>
          {logicDefs.map(x => (
            <option key={x.name} value={x.name}>{x.label ? `${x.label}（${x.name}）` : x.name}</option>
          ))}
          {!names.includes(logic) && <option value={logic}>{logic}（当前自定义）</option>}
          <option value="__custom__">— 手动输入 —</option>
        </select>
      </F>

      <F label="自定义名称（可直接输入覆盖）">
        <input
          value={logic}
          onChange={e => set('logic', e.target.value)}
          placeholder="logic_custom_xxx"
        />
      </F>

      {/* 动态渲染该 logic 的可调参数（来自 logics.json） */}
      {params.map(p => (
        <ParamField key={p.key} param={p} value={d[p.key]} onChange={v => set(p.key, v)} />
      ))}

      {curDef?.report && (
        <div className="ncp-hint">
          → 该逻辑需要连接「上报配置」节点（{curDef.report === 'dify' ? 'Dify' : 'HTTP 服务器'}）
        </div>
      )}
    </div>
  )
}

// 按参数类型动态渲染一个表单控件（int/float/string/bool/enum/text）
function ParamField({ param, value, onChange }: {
  param: LogicParam
  value: unknown
  onChange: (v: unknown) => void
}) {
  const label = param.label ?? param.key
  const hint  = param.help ? <div className="ncp-hint">{param.help}</div> : null

  if (param.type === 'bool') {
    return (
      <label className="node-toggle">
        <input type="checkbox"
          checked={value !== undefined ? !!value : !!param.default}
          onChange={e => onChange(e.target.checked)} />
        {label}
      </label>
    )
  }
  if (param.type === 'enum') {
    return (
      <F label={label}>
        <select value={value !== undefined ? String(value) : String(param.default ?? '')}
          onChange={e => onChange(e.target.value)}>
          {(param.options ?? []).map(o => <option key={o} value={o}>{o}</option>)}
        </select>
        {hint}
      </F>
    )
  }
  if (param.type === 'text') {
    return (
      <F label={label}>
        <textarea rows={4} style={{ resize: 'vertical' }}
          value={value !== undefined ? String(value) : String(param.default ?? '')}
          placeholder={param.placeholder}
          onChange={e => onChange(e.target.value)} />
        {hint}
      </F>
    )
  }
  if (param.type === 'int' || param.type === 'float') {
    return (
      <F label={label}>
        <NumberField
          value={value}
          def={param.default !== undefined ? Number(param.default) : undefined}
          min={param.min} max={param.max}
          step={param.type === 'float' ? 0.01 : 1}
          onChange={v => onChange(v)} />
        {hint}
      </F>
    )
  }
  // string
  return (
    <F label={label}>
      <input
        value={value !== undefined ? String(value) : String(param.default ?? '')}
        placeholder={param.placeholder}
        onChange={e => onChange(e.target.value)} />
      {hint}
    </F>
  )
}

// ─────────────────────────────────────────────────────────────────────────────
// Report form
// ─────────────────────────────────────────────────────────────────────────────
function ReportForm({ node, onUpdate }: { node: Node; onUpdate: Props['onUpdate'] }) {
  const d     = node.data as Record<string, unknown>
  const set   = (k: string, v: unknown) => onUpdate(node.id, { [k]: v })
  const rtype = (d.report_type as string) ?? 'server'

  return (
    <div className="ncp-form">
      <F label="上报类型">
        <select value={rtype} onChange={e => set('report_type', e.target.value)}>
          <option value="server">HTTP 服务器上报</option>
          <option value="dify">Dify AI 上报</option>
        </select>
      </F>

      {rtype === 'server' && (
        <F label="上报地址 (URL，留空=用默认)">
          <input
            value={String(d.server_url ?? '')}
            onChange={e => set('server_url', e.target.value)}
            placeholder="留空则用上报服务 config.yaml 的默认地址"
          />
        </F>
      )}

      {rtype === 'dify' && <>
        <F label="Dify 地址 (api_url，留空=用默认)">
          <input
            value={String(d.dify_api_url ?? '')}
            onChange={e => set('dify_api_url', e.target.value)}
            placeholder="http://192.168.2.98:8015"
          />
        </F>
        <F label="Dify API Key（留空=用默认）">
          <input
            type="password"
            value={String(d.dify_api_key ?? '')}
            onChange={e => set('dify_api_key', e.target.value)}
            placeholder="app-xxxxxxxxxxxxxxxx"
          />
        </F>
        <F label="提示词 (Prompt)">
          <textarea
            rows={4}
            value={String(d.dify_prompt ?? '')}
            onChange={e => set('dify_prompt', e.target.value)}
            placeholder="检测到 {label}，请分析..."
            style={{ resize: 'vertical' }}
          />
        </F>
      </>}
    </div>
  )
}

// ─────────────────────────────────────────────────────────────────────────────
// ROI info — 一个 ROI 节点(=一个通道)可包含多个命名区域; 都在节点的绘制弹窗里管理。
// ─────────────────────────────────────────────────────────────────────────────
function ROIInfo({ node }: { node: Node }) {
  const zones = useROIStore(s => s.zones[node.id] ?? EMPTY_ZONES)
  const n     = zones.length

  return (
    <div className="ncp-form">
      <div className={`ncp-roi-status ${n > 0 ? 'active' : ''}`}>
        {n > 0 ? `✔ 已配置 ${n} 个 ROI 区域` : '🔲 未绘制 ROI — 推理时使用全屏'}
      </div>
      {n > 0 && (
        <ul className="ncp-roi-zone-ul">
          {zones.map((z, i) => (
            <li key={i}>{i + 1}. {z.name?.trim() || `区域${i + 1}`}（{Math.max(0, z.polygon.length - 1)} 顶点）</li>
          ))}
        </ul>
      )}
      <div className="ncp-hint" style={{ marginTop: 10 }}>
        在画布的 ROI 节点上点「绘制/编辑 ROI 区域」，可在同一张画面上画多个区域并各自命名。
        逻辑里用 ctx-&gt;rois / ctx-&gt;roi_by_name(...) 调用各区域。
      </div>
    </div>
  )
}
