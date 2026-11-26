import type { Dispatch, SetStateAction } from 'react'
import type { AppInfo } from '../../api/apps'
import type { ImageCapabilities, ImageModel, InferenceAssets } from '../../api/imageInference'
import ModelAssetInput, { type ModelImportResult } from './ModelAssetInput'
import type { ModelCheck } from './useModelChecks'

export default function ModelEditor({ models, capabilities, apps, library, disabled, checks, onRetryCheck, onImport, onChange, selectedApps, setSelectedApps }: {
  models: ImageModel[]; capabilities: ImageCapabilities; apps: AppInfo[]; library: InferenceAssets; disabled: boolean
  checks: ModelCheck[]; onRetryCheck: () => void
  onImport: (app: AppInfo | undefined, file: File, onProgress: (value: number) => void) => Promise<ModelImportResult>
  onChange: (models: ImageModel[]) => void
  selectedApps: Record<string, string>
  setSelectedApps: Dispatch<SetStateAction<Record<string, string>>>
}) {
  const patch = (index: number, value: Partial<ImageModel>) => {
    const previousId = models[index].id
    if (value.id !== undefined && value.id !== previousId) {
      const nextId = value.id
      setSelectedApps(current => ({ ...current, [nextId]: current[previousId] }))
    }
    onChange(models.map((model, i) => i === index ? { ...model, ...value } : model))
  }
  return <div className="image-models">
    {models.map((model, index) => {
      const kind = capabilities.models.find(m => m.type === model.model_type)
      const check = checks[index]
      const app = apps.find(app => model.model_path.startsWith(`${app.path}/`))
        ?? apps.find(app => app.name === selectedApps[model.id])
        ?? apps.find(app => model.label_path.startsWith(`${app.path}/`))
      const modelPaths = app?.models.map(path => `${app.path}/${path}`) ?? library.models
      const labelPaths = app?.labels.map(path => `${app.path}/${path}`) ?? library.labels
      return <fieldset className="image-model" key={index} disabled={disabled} aria-label={`模型 ${index + 1}`}>
        <details className="image-model-settings" open>
          <summary aria-label={`模型 ${index + 1} 设置`}>
            <span>{models.length > 1 ? `模型 ${index + 1}` : '模型'}</span>
            {models.length > 1 && <button type="button" className="image-remove" aria-label={`移除模型 ${index + 1}`}
              onClick={event => {
                event.preventDefault()
                event.stopPropagation()
                onChange(models.filter((_, i) => i !== index))
              }}>移除</button>}
          </summary>
          <label>类型<select aria-label={`模型 ${index + 1} 类型`} value={model.model_type} onChange={e => patch(index, { model_type: e.target.value })}>
            {!kind && <option value={model.model_type}>{model.model_type}（不支持）</option>}
            {capabilities.models.map(m => <option key={m.type}>{m.type}</option>)}
          </select></label>
          <label>模型来源<select aria-label={`模型 ${index + 1} 来源`} value={app?.name ?? ''}
            onChange={event => {
              const name = event.target.value
              setSelectedApps(current => ({ ...current, [model.id]: name }))
              patch(index, { model_path: '', label_path: '' })
            }}>
            <option value="">独立模型库</option>
            {apps.map(app => <option key={app.name} value={app.name}>{app.name}</option>)}
          </select></label>
          <ModelAssetInput key={`model-${app?.name ?? ''}`} title={`模型 ${index + 1} 文件`} value={model.model_path} options={modelPaths}
            extension=".rknn" required onImport={(file, progress) => onImport(app, file, progress)}
            onChange={model_path => patch(index, { model_path })} />
          {model.model_path && <div className={`image-model-check${check?.error ? ' is-error' : ''}`}
            role="status" aria-label={`模型 ${index + 1} 检查`}>
            {check?.error ? <><span>{check.error}</span><button type="button" onClick={onRetryCheck}
              aria-label={`重新检查模型 ${index + 1}`}>重试</button></> : check?.info
              ? `✓ ${check.info.width} × ${check.info.height} · ${check.info.format}` : '检查中…'}
          </div>}
          <ModelAssetInput key={`label-${app?.name ?? ''}`} title={`模型 ${index + 1} 标签文件`} value={model.label_path} options={labelPaths}
            extension=".txt" required={kind?.requires_labels} onImport={!app ? (file, progress) => onImport(undefined, file, progress) : undefined} onChange={label_path => patch(index, { label_path })} />
          <details className="image-model-parameters">
            <summary aria-label={`模型 ${index + 1} 参数`}>参数</summary>
            <label>模型 ID<input aria-label={`模型 ${index + 1} ID`} value={model.id} maxLength={64}
              onChange={e => patch(index, { id: e.target.value })} required pattern="[a-z][a-z0-9_-]{0,63}" /></label>
            {kind?.result_kind !== 'depth' && <div className="image-fields">
              <label>置信度阈值<input aria-label={`模型 ${index + 1} 置信度阈值`} type="number" required min="0" max="1" step="0.01"
                value={Number.isFinite(model.obj_thresh) ? model.obj_thresh : ''} onChange={e => patch(index, { obj_thresh: e.target.valueAsNumber })} /></label>
              <label>NMS 阈值<input aria-label={`模型 ${index + 1} NMS 阈值`} type="number" required min="0" max="1" step="0.01"
                value={Number.isFinite(model.nms_thresh) ? model.nms_thresh : ''} onChange={e => patch(index, { nms_thresh: e.target.valueAsNumber })} /></label>
            </div>}
            <div className="image-fields">
              <label>检测类别<input aria-label={`模型 ${index + 1} 检测类别`} placeholder="空 = 全部，多个类别用逗号分隔" value={model.detect_classes.join(',')}
                onChange={e => patch(index, { detect_classes: e.target.value ? e.target.value.split(/[,，]/) : [] })} /></label>
              <label>NPU 核心<select aria-label={`模型 ${index + 1} NPU 核心`} value={model.npu_core} onChange={e => patch(index, { npu_core: Number(e.target.value) })}>
                <option value={-1}>自动</option>{[0, 1, 2].map(core => <option key={core} value={core}>核心 {core}</option>)}
              </select></label>
            </div>
          </details>
        </details>
      </fieldset>
    })}
    <button type="button" disabled={disabled || models.length >= capabilities.max_models} onClick={() => {
      let index = models.length + 1
      while (models.some(m => m.id === `model_${index}`)) index++
      const type = capabilities.models.find(m => m.type === models[models.length - 1]?.model_type)?.type ?? capabilities.models[0].type
      onChange([...models, { id: `model_${index}`, model_type: type,
        model_path: '', label_path: '', obj_thresh: .3, nms_thresh: .45, npu_core: -1, detect_classes: [] }])
    }} className="image-add-model">＋ 添加模型</button>
  </div>
}
