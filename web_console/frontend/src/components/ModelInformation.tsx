import { useEffect, useState } from 'react'
import { apiErrorMessage, fetchModelInfo, fetchModelRuntime, type ModelInfo, type ModelRuntime } from '../api/client'

interface Props {
  app: string | null
  path: string
  modelType: string
  modelId: string
  channelId?: number
  assetsRevision: unknown
}

export default function ModelInformation({ app, path, modelType, modelId, channelId, assetsRevision }: Props) {
  const [info, setInfo] = useState<ModelInfo | null>(null)
  const [probeError, setProbeError] = useState('')
  const [runtime, setRuntime] = useState<ModelRuntime | null>(null)
  const [runtimeError, setRuntimeError] = useState('')

  useEffect(() => {
    let cancelled = false
    setInfo(null)
    setProbeError('')
    if (app && path) {
      fetchModelInfo(app, path, modelType).then(result => {
        if (!cancelled) setInfo(result)
      }).catch(error => {
        if (!cancelled) setProbeError(apiErrorMessage(error))
      })
    }
    return () => { cancelled = true }
  }, [app, path, modelType, assetsRevision])

  useEffect(() => {
    let cancelled = false
    let timer: ReturnType<typeof setTimeout> | undefined
    setRuntime(null)
    setRuntimeError('')
    const poll = async () => {
      if (cancelled || !app) return
      if (!document.hidden) {
        try {
          const result = await fetchModelRuntime(app)
          if (!cancelled) { setRuntime(result); setRuntimeError('') }
        } catch (error) {
          if (!cancelled) { setRuntime(null); setRuntimeError(apiErrorMessage(error)) }
        }
      }
      if (!cancelled) timer = setTimeout(poll, 2000)
    }
    void poll()
    return () => { cancelled = true; clearTimeout(timer) }
  }, [app])

  const channel = runtime?.channels.find(item => item.channel_id === channelId)
  const active = channel?.active_models.find(item => item.id === modelId)
  const samePathAndType = active && active.model_path.replace(/^\.\//, '') === path.replace(/^\.\//, '')
    && active.model_type === modelType
  const fileReplaced = samePathAndType && info?.file_version && info.file_version !== active.file_version
  const sameSelection = samePathAndType && !fileReplaced
  const error = probeError || (info?.ok === false ? info.error : '')

  return <div className="model-information" aria-live="polite">
    <div><strong>业务坐标：</strong>640 × 640，中心 (320, 320)</div>
    <div><strong>所选模型输入：</strong>{!path ? '请选择模型文件' : info?.width && info?.height
      ? `${info.width} × ${info.height} · ${info.format}` : error ? '检查失败' : '读取中…'}</div>
    {error && <div className="ncp-field-error">{error}</div>}
    <div><strong>实际运行：</strong>{runtimeError || runtime?.error || (!runtime ? '查询中…'
      : runtime.status !== 'running' ? '程序未启动'
      : channelId == null ? '模型尚未连接到视频流'
      : !channel ? '该通道尚无已生效模型'
      : channel.state === 'loading' ? '正在切换模型…'
      : active ? `${active.width} × ${active.height} · ${active.model_type} · ${active.model_path}`
      : channel.active_models.length ? channel.active_models.map(item => `${item.id}: ${item.width} × ${item.height}`).join('；')
      : '该通道推理未启用或模型未加载')}</div>
    {runtime?.config && <div className="model-information-note">运行配置：{runtime.config}</div>}
    {channel?.state === 'failed' && <div className="ncp-field-error">
      加载或切换失败：{channel.error}。{channel.active_models.length ? '已生效模型仍在运行。' : '当前无已确认运行模型。'}
    </div>}
    {channel?.state === 'applied' && !sameSelection && <div className="model-information-note">
      {fileReplaced ? '模型文件已更新，当前仍为旧实例。请重启程序加载同名覆盖的模型。'
        : '所选模型尚未在该通道生效。保存正在运行的配置后，请以实际运行状态为准。'}
    </div>}
  </div>
}
