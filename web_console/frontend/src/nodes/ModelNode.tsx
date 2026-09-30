import { Handle, Position, NodeProps } from '@xyflow/react'
import { useEditorStore } from '../store/editorStore'
import './nodeStyles.css'

export default function ModelNode({ data, selected }: NodeProps) {
  const d = data as Record<string, unknown>
  const enabled   = d.infer_enable !== false   // YOLO 推理开关（不再是整条通道的 enable）
  const modelId   = String(d.id ?? '').trim()
  const modelPath = String(d.model_path ?? '')
  const labelPath = String(d.label_path ?? '')
  const modelType = String(d.model_type ?? 'yolov8_det')
  const basename  = modelPath ? modelPath.split('/').pop()! : '（未配置）'
  const assets = useEditorStore(s => s.assets)
  const assetsReady = useEditorStore(s => s.assetsStatus === 'ready'
    && s.assetsForApp === s.appName)
  const modelMissing = assetsReady && !!modelPath && !assets.models.includes(modelPath)
  const labelMissing = assetsReady && !!labelPath && !assets.labels.includes(labelPath)
  const missingText = modelMissing && labelMissing
    ? '模型和标签文件不存在'
    : modelMissing ? '模型文件不存在' : labelMissing ? '标签文件不存在' : ''

  return (
    <div className={`rf-node rf-node-compact${selected ? ' selected' : ''}`}>
      {/* left: stream input */}
      <Handle type="target" position={Position.Left} id="stream-in" />

      <div className="rf-node-header header-model">
        <span>🧠</span>
        <span className={`model-node-id${modelId ? '' : ' unset'}`}
          title={modelId || '模型业务 ID 未设置'}>
          {modelId || '未设置 ID'}
        </span>
        <span className="node-status-badge" style={{
          marginLeft: 'auto',
          color:  enabled ? '#86efac' : '#94a3b8',
          background: enabled ? 'rgba(22,163,74,0.18)' : 'rgba(100,116,139,0.15)',
          border: `1px solid ${enabled ? '#16a34a55' : '#47556955'}`,
        }}>
          {enabled ? '推理开' : '推理关'}
        </span>
      </div>
      <div className={`rf-node-summary${missingText ? ' asset-missing' : ''}`}
        title={missingText ? `${missingText}：${modelMissing ? modelPath : labelPath}` : modelPath}>
        {missingText ? `⚠ ${missingText}` : `${modelType} · ${basename}`}
      </div>

      {/* right: output to logic */}
      <Handle type="source" position={Position.Right} id="logic-out" />
    </div>
  )
}
