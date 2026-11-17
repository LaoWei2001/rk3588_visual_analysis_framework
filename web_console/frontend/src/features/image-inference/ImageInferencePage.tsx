import { useEffect, useRef, useState, type CSSProperties } from 'react'
import { useSearchParams } from 'react-router-dom'
import { apiErrorMessage } from '../../api/http'
import {
  fetchImageAssets, uploadImageAsset, resolveCanvasImageSettings, type ImageAssets, type UploadedImageAsset,
  cancelImageJob, clearCompletedImageJobs, createImageJob, createDeviceImageJob, fetchImageCapabilities, fetchCurrentImageJob,
  imageArchiveUrl, imageJobFinished, type ImageCapabilities, type ImageJob, type ImageSettings,
} from '../../api/imageInference'
import ModelEditor from './ModelEditor'
import ResultView, { statusText } from './ResultView'
import SourcePicker, { type SourceChoice } from './SourcePicker'
import { imageModel, readImagePreset } from './preset'
import useConfigSidebar from './useConfigSidebar'
import useModelChecks from './useModelChecks'
import './imageInference.css'

const EMPTY_ASSETS: ImageAssets = { models: [], labels: [] }

export default function ImageInferencePage() {
  const [query] = useSearchParams()
  const presetId = query.get('preset')
  const [assets, setAssets] = useState<ImageAssets>(EMPTY_ASSETS)
  const [capabilities, setCapabilities] = useState<ImageCapabilities | null>(null)
  const [settings, setSettings] = useState<ImageSettings>({ models: [imageModel({})] })
  const modelChecks = useModelChecks(settings.models)
  const [files, setFiles] = useState<File[]>([])
  const [source, setSource] = useState<SourceChoice>({ kind: 'upload', path: '', recursive: false })
  const [job, setJob] = useState<ImageJob | null>(null)
  const [index, setIndex] = useState(0)
  const [error, setError] = useState('')
  const [catalogError, setCatalogError] = useState('')
  const [pollError, setPollError] = useState('')
  const [submitting, setSubmitting] = useState(false)
  const [clearing, setClearing] = useState(false)
  const [importing, setImporting] = useState(false)
  const [presetLoading, setPresetLoading] = useState(false)
  const [uploadProgress, setUploadProgress] = useState(0)
  const [revision, setRevision] = useState(0)
  const { collapsed: configCollapsed, width: configWidth, resizing, workspaceRef, separatorProps, toggle: toggleConfig } = useConfigSidebar()
  const resultsRef = useRef<HTMLDivElement>(null)
  const jobMutation = useRef(0)
  const active = job !== null && !imageJobFinished(job.status)
  const hasInput = source.kind === 'upload' ? files.length > 0 : source.path.trim().length > 0
  useEffect(() => { setIndex(0) }, [job?.id])

  useEffect(() => {
    let stale = false
    const preset = readImagePreset(presetId)
    setError('')
    if (!preset) { setPresetLoading(false); setSettings({ models: [imageModel({})] }); return }
    setPresetLoading(true)
    const load = preset.app ? resolveCanvasImageSettings(preset.app, preset.settings) : Promise.resolve(preset.settings)
    void load.then(value => { if (!stale) setSettings(value) })
      .catch(error => { if (!stale) setError(apiErrorMessage(error)) })
      .finally(() => { if (!stale) setPresetLoading(false) })
    return () => { stale = true }
  }, [presetId])
  useEffect(() => {
    let stale = false
    setCapabilities(null); setAssets(EMPTY_ASSETS); setCatalogError('')
    void Promise.all([fetchImageCapabilities(), fetchImageAssets()]).then(([catalog, resources]) => {
      if (!stale) { setCapabilities(catalog); setAssets(resources) }
    }).catch(error => { if (!stale) setCatalogError(apiErrorMessage(error)) })
    return () => { stale = true }
  }, [revision])
  useEffect(() => {
    let stale = false
    let timer: ReturnType<typeof setTimeout>
    let count = 0
    let wasActive = false
    const poll = async () => {
      const mutation = jobMutation.current
      try {
        const value = await fetchCurrentImageJob()
        // A delayed poll must not undo a submission, cancellation or cleanup.
        if (stale || mutation !== jobMutation.current) return
        setJob(value); setPollError('')
        const running = value !== null && !imageJobFinished(value.status)
        if (++count % 10 === 0 || running !== wasActive) {
          wasActive = running
          const caps = await fetchImageCapabilities()
          if (!stale && mutation === jobMutation.current) setCapabilities(caps)
        }
      } catch (error) {
        if (!stale && mutation === jobMutation.current) setPollError(apiErrorMessage(error))
      } finally {
        if (!stale) timer = setTimeout(poll, 1000)
      }
    }
    void poll()
    return () => { stale = true; clearTimeout(timer) }
  }, [])

  const chooseFiles = (chosen: FileList | null) => {
    if (!chosen) return
    const next = Array.from(chosen)
    const limit = capabilities?.max_images ?? 100
    if (next.length > limit || next.some(f => f.size > (capabilities?.max_image_bytes ?? 20 * 1024 ** 2))
        || next.reduce((sum, f) => sum + f.size, 0) > (capabilities?.max_upload_bytes ?? 256 * 1024 ** 2)) {
      setError(`每批最多 ${limit} 张；单张限 20 MiB，总大小限 256 MiB。`)
      return
    }
    setFiles(next); setError('')
  }
  const start = async (event: React.FormEvent) => {
    event.preventDefault()
    if (!capabilities || !hasInput || submitting || clearing || importing || presetLoading || !modelChecks.ready) return
    setError('')
    if (new Set(settings.models.map(m => m.id)).size !== settings.models.length) { setError('各模型 ID 必须唯一。'); return }
    if (settings.models.some(m => !Number.isFinite(m.obj_thresh) || !Number.isFinite(m.nms_thresh))) {
      setError('请填写有效的置信度和 NMS 阈值。'); return
    }
    setSubmitting(true); setUploadProgress(0)
    try {
      const options = { ...settings, models: settings.models.map(m => ({ ...m,
        detect_classes: m.detect_classes.map(c => c.trim()).filter(Boolean) })) }
      const value = source.kind === 'device'
        ? await createDeviceImageJob(options, source.path.trim(), source.recursive)
        : await createImageJob(options, files, setUploadProgress)
      jobMutation.current += 1
      setJob(value); setIndex(0)
      requestAnimationFrame(() => resultsRef.current?.scrollIntoView({ behavior: 'smooth', block: 'start' }))
    } catch (error) { setError(apiErrorMessage(error)) }
    finally { setSubmitting(false) }
  }
  const cancel = async () => {
    if (!job) return
    try {
      const value = await cancelImageJob(job.id)
      jobMutation.current += 1
      setJob(current => current?.id === value.id ? value : current)
    }
    catch (error) { setError(apiErrorMessage(error)) }
  }
  const clearRecords = async () => {
    if (clearing || submitting) return
    if (!window.confirm('确定清除全部已结束的推理记录？\n\n将删除对应的图片副本、推理图和下载包，无法恢复。\n模型、板端原图和运行中的任务会保留。')) return
    setClearing(true); setError('')
    jobMutation.current += 1
    try {
      const value = await clearCompletedImageJobs()
      jobMutation.current += 1
      setJob(value.current); setPollError('')
    } catch (error) { setError(apiErrorMessage(error)) }
    finally { setClearing(false) }
  }
  const importAsset = async (file: File, onProgress: (value: number) => void): Promise<UploadedImageAsset> => {
    setImporting(true); setError('')
    try {
      const uploaded = await uploadImageAsset(file, onProgress)
      const kind = file.name.toLowerCase().endsWith('.rknn') ? 'models' : 'labels'
      // The upload response confirms storage. A later library refresh must not
      // turn a successful upload into a failure or prevent selecting its path.
      setAssets(current => ({ ...current, [kind]: [...new Set([...current[kind], uploaded.path])].sort() }))
      return uploaded
    }
    finally { setImporting(false) }
  }
  const editingDisabled = submitting || clearing || importing || presetLoading
  return <div className="image-inference-page" role="region" aria-label="图片推理工作区" tabIndex={0}>
    <div className="image-page-content">
      <header className="image-page-header"><h1>图片推理</h1></header>
      {(error || catalogError) && <div className="image-error" role="alert">{error || catalogError}
        {catalogError && <button type="button" disabled={editingDisabled} onClick={() => setRevision(r => r + 1)}>重试</button>}
      </div>}
      {pollError && <div className="image-warning" role="status">状态更新失败，正在重试：{pollError}</div>}
      {capabilities?.busy && !active && <div className="image-warning">NPU 占用中，请先停止视频程序。</div>}
      <div ref={workspaceRef} className={`image-workspace${configCollapsed ? ' is-config-collapsed' : ''}${resizing ? ' is-config-resizing' : ''}`}
        style={{ '--image-config-width': `${configWidth}px` } as CSSProperties}>
        <button type="button" className="image-config-toggle" aria-controls="image-inference-config"
          aria-expanded={!configCollapsed} aria-label={configCollapsed ? '展开模型配置' : '收起模型配置'}
          title={configCollapsed ? '展开模型配置' : '收起模型配置'}
          onClick={toggleConfig}>
          <svg width="18" height="18" viewBox="0 0 20 20" fill="none" stroke="currentColor" strokeWidth="1.4"
            strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
            <rect x="2" y="3" width="16" height="14" rx="2" /><path d="M7 3v14" />
            <path d={configCollapsed ? 'm11 7 3 3-3 3' : 'm14 7-3 3 3 3'} />
          </svg>
        </button>
        <form id="image-inference-config" className="image-config" hidden={configCollapsed} onSubmit={start} onInvalidCapture={event => {
          // Reveal invalid advanced fields before native form validation focuses them.
          let parent = (event.target as HTMLElement).parentElement
          while (parent && parent !== event.currentTarget) {
            if (parent instanceof HTMLDetailsElement) parent.open = true
            parent = parent.parentElement
          }
        }}>
          {capabilities ? <ModelEditor models={settings.models} capabilities={capabilities} assets={assets}
            checks={modelChecks.checks} onRetryCheck={modelChecks.retry}
            disabled={editingDisabled} onUpload={importAsset}
            onChange={models => setSettings(current => ({ ...current, models }))} /> : <p className="image-note">{catalogError ? '模型暂不可用' : '加载模型…'}</p>}
          {settings.inference_roi?.mode && <div className="image-roi-note">
            已使用通道推理区域
            <button type="button" disabled={editingDisabled} onClick={() => setSettings(current => ({ ...current, inference_roi: undefined }))}>改为整图</button>
          </div>}
          {presetLoading && <p className="image-note" role="status">读取参数…</p>}
          <SourcePicker source={source} files={files} disabled={editingDisabled} onChange={setSource}
            onFiles={chooseFiles} onClear={() => setFiles([])} />
          <button className="image-primary" type="submit" aria-label={`开始推理 · ${settings.models.length} 个模型`}
            disabled={!capabilities || !hasInput || active || capabilities.busy || editingDisabled || !modelChecks.ready}>
            {submitting ? source.kind === 'device' ? '读取图片…' : `上传中 ${uploadProgress}%` : active ? '推理中…' : '开始推理'}
          </button>
        </form>
        <div className="image-config-resizer" {...separatorProps} />
        <div className="image-results-area" ref={resultsRef}>
          <div className="image-results-heading"><h2>结果</h2>
            {job && imageJobFinished(job.status) && <div className="image-result-actions">
              <a href={imageArchiveUrl(job.id)} aria-label="下载整批结果" download>下载结果</a>
              <button type="button" className="image-clear-results" aria-label="清除记录" aria-busy={clearing}
                title={clearing ? '正在清除记录…' : '清除全部已结束的记录及结果缓存'}
                disabled={clearing || submitting} onClick={() => void clearRecords()}>
                <svg width="17" height="17" viewBox="0 0 20 20" fill="none" stroke="currentColor" strokeWidth="1.4"
                  strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
                  <path d="M3 5h14M7 5V3h6v2M5 5l1 12h8l1-12M8 8v6M12 8v6" />
                </svg>
              </button>
            </div>}
          </div>
          {job ? <>
            {!imageJobFinished(job.status) && <div className="image-progress" role="status">
              <span>{statusText[job.status] ?? job.status} · {job.completed}/{job.total}</span>
              <progress aria-label="推理进度" value={job.completed} max={job.total} />
              <button type="button" disabled={job.status === 'cancelling'} onClick={() => void cancel()}>取消</button>
            </div>}
            {job.error && <div className="image-error" role="alert">{job.error}</div>}
            <div className={`image-results-workspace${job.items.length > 1 ? ' has-image-list' : ''}`}>
              <div className="image-result-preview">
                {job.items[index] && <ResultView key={`${job.id}-${index}`} job={job} item={job.items[index]} />}
              </div>
              {job.items.length > 1 && <aside className="image-item-sidebar" key={job.id}>
                <div className="image-item-heading"><span>图片</span><span>{index + 1} / {job.items.length}</span></div>
                <div className="image-item-list" role="group" aria-label="批量图片列表" tabIndex={0}>
                  {job.items.map(item => <button type="button" key={item.index} title={item.name}
                    aria-label={`${item.index + 1}. ${item.name} · ${statusText[item.status] ?? item.status}`}
                    aria-pressed={item.index === index} className={item.index === index ? 'selected' : ''}
                    onClick={() => setIndex(item.index)}>
                    <span className="image-item-number">{item.index + 1}</span>
                    <span className="image-item-name">{item.name}</span>
                    <small className={`image-status ${item.status}`}>{item.status === 'partial' ? '部分失败' : statusText[item.status] ?? item.status}</small>
                  </button>)}
                </div>
              </aside>}
            </div>
          </> : <div className="image-empty image-welcome">选择图片，开始推理</div>}
        </div>
      </div>
    </div>
  </div>
}
