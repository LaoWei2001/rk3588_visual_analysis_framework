/**
 * NodeConfigPanel — 右侧侧边栏，单击节点后显示该节点的完整配置表单。
 * 表单数据通过 onUpdate(nodeId, patch) 回调写回父组件的 nodes 状态。
 */

import { useState, useEffect, type ReactNode } from 'react'
import { Node } from '@xyflow/react'
import { useEditorStore }  from '../store/editorStore'
import { useConsoleStore } from '../store/consoleStore'
import { useROIStore, type Zone } from '../store/roiStore'
import {
  fetchAppLogics, asLogicDef, uploadAsset, deleteAsset,
  type LogicDef, type LogicParam, type ReportField, type BusinessField,
} from '../api/client'
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
const DEFAULT_USB_DEVICE = '/dev/video81'

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
  sop:    ['🧭', 'SOP流程节点'],
  report: ['📡', '上报配置节点'],
}

const HEADER_CLASS: Record<string, string> = {
  stream: 'header-stream',
  model:  'header-model',
  roi:    'header-roi',
  logic:  'header-logic',
  sop:    'header-sop',
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
    <div className={`ncp ${node.type === 'report' ? 'ncp-report' : ''}`}>
      <div className={`ncp-header ${headerCls}`}>
        <span>{icon}</span>
        <span>{title}</span>
        <span className="ncp-node-id">#{node.id.split('-')[1]}</span>
      </div>

      <div className="ncp-body">
        {node.type === 'stream' && <StreamForm  node={node} onUpdate={onUpdate} />}
        {node.type === 'model'  && <ModelForm   node={node} onUpdate={onUpdate} />}
        {node.type === 'logic'  && <LogicForm   node={node} onUpdate={onUpdate} />}
        {node.type === 'sop'    && <SopInfo     node={node} onUpdate={onUpdate} />}
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
    if (type === 'usb')  { patch.device = d.device ?? DEFAULT_USB_DEVICE }
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
            value={String(d.device ?? DEFAULT_USB_DEVICE)}
            onChange={e => set('device', e.target.value)}
            placeholder={DEFAULT_USB_DEVICE}
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
  const appName = useEditorStore(s => s.appName)
  const [logicDefs, setLogicDefs] = useState<LogicDef[]>([])
  const [catalogLoading, setCatalogLoading] = useState(false)
  const [catalogSource, setCatalogSource] = useState<'catalog' | 'binary' | 'unavailable' | null>(null)
  const [catalogError, setCatalogError] = useState<string | null>(null)

  useEffect(() => {
    let cancelled = false
    setLogicDefs([])
    setCatalogSource(null)
    setCatalogError(null)
    setCatalogLoading(false)
    if (!appName) return () => { cancelled = true }
    setCatalogLoading(true)
    fetchAppLogics(appName)
      .then(res => {
        if (cancelled) return
        setLogicDefs(res.channel_logics.map(asLogicDef))
        setCatalogSource(res.source)
        setCatalogError(res.error ?? null)
      })
      .catch(() => {
        if (cancelled) return
        setLogicDefs([])
        setCatalogSource('unavailable')
        setCatalogError('无法从后端读取当前应用的通道逻辑清单')
      })
      .finally(() => {
        if (!cancelled) setCatalogLoading(false)
      })
    return () => { cancelled = true }
  }, [appName])

  const d      = node.data as Record<string, unknown>
  const logic  = String(d.logic ?? '')
  const names  = logicDefs.map(x => x.name)
  const curDef = logicDefs.find(x => x.name === logic)
  const params = curDef?.params ?? []

  // 切换 logic：模块参数换成新 Schema 的默认集合，不能遗留旧 logic 的未知键。
  const selectLogic = (name: string) => {
    const def = logicDefs.find(x => x.name === name)
    const patch: Record<string, unknown> = { logic: name }
    const moduleParameters: Record<string, unknown> = {}
    ;(def?.params ?? []).forEach(p => {
      if (p.storage === 'logic_parameters') {
        if (p.default !== undefined) moduleParameters[p.key] = p.default
      } else if (d[p.key] === undefined && p.default !== undefined) {
        patch[p.key] = p.default
      }
    })
    patch.logic_parameters = moduleParameters
    onUpdate(node.id, patch)
  }

  return (
    <div className="ncp-form">
      {/* 逻辑名称只能从当前应用动态返回的清单里选，不允许手动输入新名字 ——
          逻辑的"身份"是 REGISTER_LOGIC 注册的字符串，完整清单来自 logics.json，
          手填任意名只会让运行时 channel_logic_get 查不到、通道空跑。详见
          docs/skills/rk3588-channel-logic/references/logic-naming-and-registration.md */}
      <F label="逻辑名称（从当前应用动态读取）">
        <select value={logic} disabled={catalogLoading || logicDefs.length === 0}
          onChange={e => selectLogic(e.target.value)}>
          <option value="">（未选择后处理逻辑）</option>
          {logicDefs.map(x => (
            <option key={x.name} value={x.name}>{x.label ? `${x.label}（${x.name}）` : x.name}</option>
          ))}
          {logic && !names.includes(logic) && (
            <option value={logic}>{logic}（⚠ 不在当前应用清单，请重新选择）</option>
          )}
        </select>
      </F>

      {catalogLoading && <div className="ncp-hint">正在读取当前应用的逻辑清单…</div>}
      {!catalogLoading && catalogError && (
        <div className="ncp-hint">⚠ 逻辑清单不可用：{catalogError}</div>
      )}
      {!catalogLoading && !catalogError && logicDefs.length === 0 && (
        <div className="ncp-hint">⚠ 当前应用没有声明任何通道逻辑。</div>
      )}
      {!catalogLoading && catalogSource === 'binary' && (
        <div className="ncp-hint">
          当前仅从二进制读取到逻辑名称；参数、动作和字段信息需要应用包中的 logics.json。
        </div>
      )}

      {/* 动态渲染该 logic 的可调参数（来自 logics.json） */}
      <LogicParameterFields node={node} params={params} onUpdate={onUpdate} />

      {curDef?.report && (
        <div className="ncp-hint">
          → 该逻辑需要连接「上报配置」节点（{curDef.report === 'dify' ? 'Dify' : 'HTTP 服务器'}）
        </div>
      )}
    </div>
  )
}

function LogicParameterFields({ node, params, onUpdate }: {
  node: Node
  params: LogicParam[]
  onUpdate: Props['onUpdate']
}) {
  const data = node.data as Record<string, unknown>
  const moduleParameters = data.logic_parameters && typeof data.logic_parameters === 'object' &&
    !Array.isArray(data.logic_parameters)
    ? data.logic_parameters as Record<string, unknown> : {}

  const valueOf = (param: LogicParam): unknown => param.storage === 'logic_parameters'
    ? moduleParameters[param.key] : data[param.key]
  const setParam = (param: LogicParam, value: unknown) => {
    if (param.storage === 'logic_parameters') {
      onUpdate(node.id, {
        logic_parameters: { ...moduleParameters, [param.key]: value },
      })
    } else {
      onUpdate(node.id, { [param.key]: value })
    }
  }

  return <>
    {params.map(param => (
      <ParamField
        key={`${param.storage ?? 'channel'}:${param.key}`}
        param={param}
        value={valueOf(param)}
        onChange={value => setParam(param, value)}
      />
    ))}
  </>
}

// 按参数类型动态渲染一个表单控件（int/float/string/bool/enum/text）
function ParamField({ param, value, onChange }: {
  param: LogicParam
  value: unknown
  onChange: (v: unknown) => void
}) {
  const label = `${param.label ?? param.key}${param.unit ? `（${param.unit}）` : ''}`
  const reloadHint = param.hot_reload === 'restart_required'
    ? '修改后需要重启程序。'
    : param.hot_reload === 'reset_state'
      ? '修改后会清空当前通道的逻辑状态。' : ''
  const hintText = [param.help, reloadHint].filter(Boolean).join(' ')
  const hint  = hintText ? <div className="ncp-hint">{hintText}</div> : null

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
  if (param.type === 'json') {
    return <JsonParamField param={param} value={value} onChange={onChange} label={label} hint={hint} />
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
          value={value ?? param.default}
          def={param.default !== undefined ? Number(param.default) : undefined}
          min={param.min} max={param.max}
          step={param.step ?? (param.type === 'float' ? 0.01 : 1)}
          integerOnly={param.type === 'int'}
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

function JsonParamField({ param, value, onChange, label, hint }: {
  param: LogicParam
  value: unknown
  onChange: (v: unknown) => void
  label: string
  hint: ReactNode
}) {
  const render = (input: unknown) => JSON.stringify(input ?? param.default ?? null, null, 2)
  const [text, setText] = useState(render(value))
  const [invalid, setInvalid] = useState(false)
  useEffect(() => setText(render(value)), [value]) // eslint-disable-line react-hooks/exhaustive-deps

  return (
    <F label={label}>
      <textarea rows={5} style={{ resize: 'vertical' }} value={text}
        onChange={event => {
          const next = event.target.value
          setText(next)
          try {
            const parsed = JSON.parse(next)
            const matchesContainer = param.json_type === 'array'
              ? Array.isArray(parsed)
              : param.json_type === 'object'
                ? parsed !== null && typeof parsed === 'object' && !Array.isArray(parsed)
                : true
            if (!matchesContainer) {
              setInvalid(true)
              return
            }
            setInvalid(false)
            onChange(parsed)
          } catch {
            setInvalid(true)
          }
        }} />
      {invalid && <div className="ncp-hint" style={{ color: '#ef4444' }}>
        JSON 格式不完整或容器类型不符，尚未保存该输入。
      </div>}
      {hint}
    </F>
  )
}

// ─────────────────────────────────────────────────────────────────────────────
// Report form
// ─────────────────────────────────────────────────────────────────────────────
type DeliveryInput = { key: string; source: string; value?: unknown; type?: string; required?: boolean }
type EventFieldMapping = {
  source: string
  target: string
  type?: BusinessField['type']
  required?: boolean
}
type Delivery = {
  id: string; enabled: boolean; media: 'image' | 'video'; target: 'server' | 'dify'
  profile_id?: string; file_variable?: string; file_input_mode?: 'single' | 'list'; event_variable?: string; inputs: DeliveryInput[]
  event_fields?: EventFieldMapping[]
  server_source?: string; server_event_type?: string
}

type SourceField = ReportField & { source: string }
const BUILTIN_REPORT_FIELDS: SourceField[] = [
  { source: 'event.id', key: 'event_id', type: 'string', label: '报警事件ID' },
  { source: 'event.type', key: 'alarm_type', type: 'string', label: '报警类型' },
  { source: 'event.message', key: 'message', type: 'string', label: '报警说明' },
  { source: 'event.trigger_time', key: 'trigger_time', type: 'number', label: '报警时间戳' },
  { source: 'channel.id', key: 'channel_id', type: 'number', label: '视频通道ID' },
]

function ReportForm({ node, onUpdate }: { node: Node; onUpdate: Props['onUpdate'] }) {
  const d     = node.data as Record<string, unknown>
  const appName = useEditorStore(s => s.appName)
  const uploadProfiles = useEditorStore(s => s.uploadProfiles)
  const [logicDefs, setLogicDefs] = useState<LogicDef[]>([])
  useEffect(() => {
    if (!appName) return
    fetchAppLogics(appName)
      .then(result => setLogicDefs(result.channel_logics.map(asLogicDef)))
      .catch(() => setLogicDefs([]))
  }, [appName])

  const set   = (k: string, v: unknown) => onUpdate(node.id, { [k]: v })
  const policy = (d.report_policy && typeof d.report_policy === 'object'
    ? d.report_policy : {}) as Record<string, unknown>
  const storedDeliveries = (Array.isArray(policy.deliveries) ? policy.deliveries : []) as Delivery[]
  const deliveries: Delivery[] = [storedDeliveries[0] ?? {
    id: `delivery_${node.id}`, enabled: true, media: 'image', target: 'server', inputs: [],
  }]
  const delivery = deliveries[0]

  const logicName = String(d.logic_name ?? '')
  const logicDef = logicDefs.find(item => item.name === logicName)
  const sourceFields: SourceField[] = [
    ...BUILTIN_REPORT_FIELDS,
    ...((logicDef?.report_fields ?? []).map(field => ({ ...field, source: `logic.${field.key}` }))),
  ]
  const businessFields = logicDef?.business_fields ?? []
  const hasExplicitEventFields = Array.isArray(delivery.event_fields)
  const effectiveEventFields: EventFieldMapping[] = hasExplicitEventFields
    ? delivery.event_fields ?? []
    : businessFields
        .filter(field => field.default_selected !== false)
        .map(field => ({
          source: field.path,
          target: field.path,
          type: field.type,
          required: field.required === true,
        }))
  const allEventFieldsSelected = businessFields.length > 0
    && businessFields.every(field => effectiveEventFields.some(item => item.source === field.path))

  const setPolicy = (patch: Record<string, unknown>) => set('report_policy', { ...policy, ...patch })
  const setDeliveries = (next: Delivery[]) => setPolicy({ deliveries: next.slice(0, 1) })
  const patchDelivery = (patch: Partial<Delivery>) => setDeliveries([{ ...delivery, ...patch }])
  const replaceInput = (source: string, next?: DeliveryInput) => {
    const inputs = (delivery.inputs ?? []).filter(item => item.source !== source)
    if (next) inputs.push(next)
    patchDelivery({ inputs })
  }
  const replaceEventField = (source: string, next?: EventFieldMapping) => {
    const mappings = effectiveEventFields.filter(item => item.source !== source)
    if (next) mappings.push(next)
    const fieldOrder = new Map(businessFields.map((field, index) => [field.path, index]))
    mappings.sort((a, b) => (fieldOrder.get(a.source) ?? 9999) - (fieldOrder.get(b.source) ?? 9999))
    patchDelivery({ event_fields: mappings })
  }
  const toggleAllEventFields = () => patchDelivery({
    event_fields: allEventFieldsSelected ? [] : businessFields.map(field => {
      const current = effectiveEventFields.find(item => item.source === field.path)
      return {
        source: field.path,
        target: current?.target || field.path,
        type: field.type,
        required: field.required === true,
      }
    }),
  })
  const clearOptionalEventFields = () => patchDelivery({
    event_fields: businessFields
      .filter(field => field.required)
      .map(field => ({
        source: field.path,
        target: effectiveEventFields.find(item => item.source === field.path)?.target || field.path,
        type: field.type,
        required: true,
      })),
  })
  const independentMappingRows = sourceFields.map(field => {
    const mapping = (delivery.inputs ?? []).find(item => item.source === field.source)
    return <div key={field.source} className="report-mapping-row">
      <div className="report-map-main">
        <div className="report-map-field">
          <label>算法/事件参数（只读）</label>
          <input disabled value={`${field.label ?? field.key} · ${field.key} (${field.type})`} />
        </div>
        <div className="report-map-field">
          <label>Dify 顶层输入变量名（留空则不单独发送）</label>
          <input value={mapping?.key ?? ''} placeholder={field.key}
            onChange={e => replaceInput(field.source, e.target.value.trim() ? {
              key: e.target.value, source: field.source, type: field.type,
            } : undefined)} />
        </div>
      </div>
    </div>
  })
  const reportKind = delivery.target === 'server' ? 'server_image'
    : delivery.media === 'video' ? 'dify_video' : 'dify_image'
  const changeKind = (kind: string) => {
    const media: 'image' | 'video' = kind === 'dify_video' ? 'video' : 'image'
    const target: 'server' | 'dify' = kind === 'server_image' ? 'server' : 'dify'
    const profile = uploadProfiles[delivery.profile_id ?? '']
    patchDelivery({
      media, target,
      profile_id: profile?.type && profile.type !== target ? '' : delivery.profile_id,
      file_variable: target === 'dify' ? (media === 'video' ? 'video' : 'image') : undefined,
      file_input_mode: target === 'dify' ? 'single' : undefined,
      event_variable: target === 'dify' && logicName === 'logic_path_sop' ? 'event_json' : undefined,
      inputs: [],
    })
  }

  return (
    <div className="ncp-form">
      <div className="ncp-hint">一个节点对应一种固定投递。SOP业务JSON可在这里选择源字段、修改最终JSON路径并映射到Dify输入变量。</div>
      <div className="report-delivery-card">
          <F label="上报类型">
            <select value={reportKind} onChange={e => changeKind(e.target.value)}>
              <option value="dify_image">图片 → Dify 工作流</option>
              <option value="dify_video">视频片段 → Dify 工作流</option>
              <option value="server_image">图片 → 业务服务器</option>
            </select>
          </F>
          <F label="发送连接（在“服务配置”中管理地址）">
            <select value={delivery.profile_id ?? ''}
              onChange={e => patchDelivery({ profile_id: e.target.value })}>
              <option value="">使用默认{delivery.target === 'server' ? '服务器' : ' Dify'}配置</option>
              {delivery.profile_id && !uploadProfiles[delivery.profile_id] && (
                <option value={delivery.profile_id}>{delivery.profile_id}（Profile 不存在）</option>
              )}
              {Object.entries(uploadProfiles)
                .filter(([, profile]) => !profile.type || profile.type === delivery.target)
                .map(([id, profile]) => (
                  <option key={id} value={id}>
                    {id} — {delivery.target === 'server' ? (profile.url || '未配置地址') : (profile.api_url || '未配置地址')}
                  </option>
                ))}
            </select>
          </F>
          {delivery.target === 'dify' ? <>
            <F label="Dify 文件输入变量名">
              <input value={delivery.file_variable ?? (delivery.media === 'video' ? 'video' : 'image')}
                onChange={e => patchDelivery({ file_variable: e.target.value })} />
            </F>
            <F label="Dify 文件变量类型">
              <select value={delivery.file_input_mode ?? 'single'}
                onChange={e => patchDelivery({ file_input_mode: e.target.value as 'single' | 'list' })}>
                <option value="single">单文件（File）</option>
                <option value="list">文件列表（Array[File]）</option>
              </select>
            </F>
            {logicName === 'logic_path_sop' && (
              <F label="Dify SOP业务JSON变量名">
                <input value={delivery.event_variable ?? 'event_json'} placeholder="event_json"
                  onChange={e => patchDelivery({ event_variable: e.target.value.trim() })} />
              </F>
            )}
            {!logicName && <div className="report-mapping-help">请先把上报节点连接到逻辑节点。</div>}
            {logicName && !logicDef && <div className="report-mapping-help">逻辑 {logicName} 未在 logics.json 声明字段。</div>}
            {logicName === 'logic_path_sop' && logicDef && <>
              <div className="report-section-title">SOP业务JSON字段选择与路径映射</div>
              <div className="report-mapping-help">
                字段目录由当前 App 的 <code>logics.json</code> 自动提供。勾选项会组装进
                <code>{delivery.event_variable ?? 'event_json'}</code>；“最终JSON路径”支持点号嵌套，
                例如将 <code>sop.zone_history</code> 改为 <code>process.history</code>。
                {!hasExplicitEventFields && ' 当前为兼容模式：未保存显式映射时默认发送完整业务JSON。'}
              </div>
              {businessFields.length > 0 ? <>
                <div className="report-event-actions">
                  <span>已选择 {effectiveEventFields.length} / {businessFields.length}</span>
                  <button type="button" className="report-event-button" onClick={toggleAllEventFields}>
                    {allEventFieldsSelected ? '取消全选' : '全选'}
                  </button>
                  <button type="button" className="report-event-button" onClick={clearOptionalEventFields}>只保留必填</button>
                  <button type="button" className="report-event-button"
                    onClick={() => patchDelivery({ event_fields: undefined })}>恢复默认</button>
                </div>
                {businessFields.map(field => {
                  const mapping = effectiveEventFields.find(item => item.source === field.path)
                  const selected = Boolean(mapping)
                  return <div key={field.path}
                    className={`report-event-field ${selected ? 'selected' : ''}`}
                    title={field.help}>
                    <label className="report-event-check">
                      <input type="checkbox" checked={selected}
                        onChange={e => replaceEventField(field.path, e.target.checked ? {
                          source: field.path,
                          target: field.path,
                          type: field.type,
                          required: field.required === true,
                        } : undefined)} />
                      <span>
                        <strong>{field.label ?? field.path}</strong>
                        <code>{field.path}</code>
                      </span>
                      <em>{field.required ? '关键字段' : field.type}</em>
                    </label>
                    <div className="report-event-target">
                      <label>最终JSON路径</label>
                      <input disabled={!selected} value={mapping?.target ?? ''} placeholder={field.path}
                        onChange={e => replaceEventField(field.path, {
                          ...mapping!, source: field.path, target: e.target.value,
                          type: field.type, required: field.required === true,
                        })} />
                    </div>
                  </div>
                })}
              </> : (
                <div className="report-mapping-help">当前 logic 未声明 business_fields，仍按旧配置发送完整业务JSON。</div>
              )}
              <details className="report-extra-inputs">
                <summary>额外独立Dify输入变量（可选）</summary>
                <div className="report-mapping-help">
                  这里的字段位于 <code>event_json</code> 之外，只在Dify确实还需要独立顶层输入变量时配置。
                </div>
                {independentMappingRows}
              </details>
            </>}
            {logicName !== 'logic_path_sop' && <>
              <div className="report-section-title">只读参数清单与Dify字段映射</div>
              {independentMappingRows}
            </>}
          </> : <>
            <div className="report-section-title">服务器固定 JSON</div>
            <div className="report-mapping-help">服务器不接收任何算法参数；仅允许修改 source 和 eventType。</div>
            <F label="source">
              <input value={delivery.server_source ?? 'JNU'}
                onChange={e => patchDelivery({ server_source: e.target.value, inputs: [] })} />
            </F>
            <F label="eventType">
              <input value={delivery.server_event_type ?? '4005'}
                onChange={e => patchDelivery({ server_event_type: e.target.value, inputs: [] })} />
            </F>
            <div className="report-map-preview">detResult = {'{}'}（固定）</div>
            <div className="report-map-preview">snapTime / endTime / eventId = 系统事件信息（固定）</div>
            <div className="report-map-preview">base64Data / base64DataRaw = 系统图片（固定）</div>
            <div className="report-map-preview">invadeFlag = 1（固定）</div>
          </>}
      </div>
      {delivery.media === 'image' && (
        <div className="report-advanced-section">
          <F label="上报图片叠加内容">
            <select value={String(policy.image_overlay ?? 'custom')}
              onChange={e => setPolicy({ image_overlay: e.target.value })}>
              <option value="none">当前原始帧</option>
              <option value="custom">与实时播放窗口画面一致</option>
            </select>
          </F>
        </div>
      )}

      {delivery.media === 'video' && (
        <div className="report-advanced-section">
          <F label="上报视频叠加内容">
            <select value={String(policy.video_overlay ?? 'custom')}
              onChange={e => setPolicy({ video_overlay: e.target.value })}>
              <option value="none">原始视频片段</option>
              <option value="custom">与实时播放窗口画面一致</option>
            </select>
          </F>
          <F label="报警前时长 (秒)">
            <NumberField min={0} max={120} step={0.5} def={3}
              value={policy.video_pre_sec ?? 3}
              onChange={v => setPolicy({ video_pre_sec: v ?? 3 })} />
          </F>
          <F label="报警后时长 (秒)">
            <NumberField min={0} max={120} step={0.5} def={3}
              value={policy.video_post_sec ?? 3}
              onChange={v => setPolicy({ video_post_sec: v ?? 3 })} />
          </F>
          <F label="录像帧率 (FPS)">
            <NumberField min={1} max={30} step={1} def={15}
              value={policy.video_fps ?? 15}
              onChange={v => setPolicy({ video_fps: v ?? 15 })} />
          </F>
        </div>
      )}
      <details className="report-advanced-section">
        <summary>配置 JSON 预览</summary>
        <pre style={{ whiteSpace: 'pre-wrap', fontSize: 11 }}>{JSON.stringify({ report_policy: policy }, null, 2)}</pre>
      </details>
    </div>
  )
}

// ─────────────────────────────────────────────────────────────────────────────
// ROI info — 一个 ROI 节点连接一个视频流通道，可包含多个命名区域。
// ─────────────────────────────────────────────────────────────────────────────
function ROIInfo({ node }: { node: Node }) {
  const zones = useROIStore(s => s.zones[node.id] ?? EMPTY_ZONES)
  const n     = zones.length

  return (
    <div className="ncp-form">
      <div className={`ncp-roi-status ${n > 0 ? 'active' : ''}`}>
        {n > 0 ? `✔ 已配置 ${n} 个 ROI 区域` : '🔲 尚未绘制 ROI 区域'}
      </div>
      {n > 0 && (
        <ul className="ncp-roi-zone-ul">
          {zones.map((z, i) => (
            <li key={i}>{i + 1}. {z.name?.trim() || `区域${i + 1}`}（{z.polygon.length} 顶点）</li>
          ))}
        </ul>
      )}
      <div className="ncp-hint" style={{ marginTop: 10 }}>
        将 ROI 节点连接到视频流节点后，点「绘制/编辑 ROI 区域」即可配置该通道的多个命名区域。
        ROI 与模型推理解耦；无论是否连接模型，逻辑里都可用
        ctx-&gt;rois / ctx-&gt;roi_by_name(...) 访问。
      </div>
    </div>
  )
}

// ─────────────────────────────────────────────────────────────────────────────
// SOP 流程信息 — 具体步骤在画布的 SOP 节点上点「配置流程」编辑(区域来自上游 ROI 节点)
// ─────────────────────────────────────────────────────────────────────────────
function SopInfo({ node, onUpdate }: { node: Node; onUpdate: Props['onUpdate'] }) {
  const appName = useEditorStore(s => s.appName)
  const [moduleParams, setModuleParams] = useState<LogicParam[]>([])
  useEffect(() => {
    if (!appName) return
    fetchAppLogics(appName)
      .then(result => {
        const def = result.channel_logics.map(asLogicDef)
          .find(item => item.name === 'logic_path_sop')
        setModuleParams((def?.params ?? []).filter(
          param => param.storage === 'logic_parameters'))
      })
      .catch(() => setModuleParams([]))
  }, [appName])

  const d     = node.data as { target_label?: string; steps?: { zoneName?: string }[] }
  const steps = d.steps ?? []

  return (
    <div className="ncp-form">
      <div className="node-field">
        <label>目标类别</label>
        <div className={`ncp-roi-status ${d.target_label?.trim() ? 'active' : ''}`}>
          {d.target_label?.trim() || '（未设置）'}
        </div>
      </div>
      <div className="node-field">
        <label>步骤（{steps.length}）</label>
        {steps.length > 0 ? (
          <ul className="ncp-roi-zone-ul">
            {steps.map((s, i) => <li key={i}>{i + 1}. {s.zoneName?.trim() || '未选区域'}</li>)}
          </ul>
        ) : (
          <div className="ncp-roi-status">还没有步骤</div>
        )}
      </div>
      <div className="ncp-hint" style={{ marginTop: 10 }}>
        在画布的 SOP 节点上点「⚙ 配置流程」编排步骤(选区域 + 每步独立参数)；
        区域沿用上游连接的「ROI 区域」节点。
      </div>
      {moduleParams.length > 0 && (
        <>
          <div className="ncp-section-title" style={{ marginTop: 14 }}>模块扩展参数</div>
          <LogicParameterFields node={node} params={moduleParams} onUpdate={onUpdate} />
        </>
      )}
    </div>
  )
}
