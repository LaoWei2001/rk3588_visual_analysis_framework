import { useState } from 'react'
import { copyText } from '../utils/clipboard'
import './ConfigPreviewPanel.css'

interface Props {
  fileName: string
  json: string | null
}

export default function ConfigPreviewPanel({ fileName, json }: Props) {
  const [copyStatus, setCopyStatus] = useState<'idle' | 'copied' | 'failed'>('idle')

  const copy = async () => {
    if (!json) return
    const success = await copyText(json)
    setCopyStatus(success ? 'copied' : 'failed')
    window.setTimeout(() => setCopyStatus('idle'), 1800)
  }

  const copyLabel = copyStatus === 'copied'
    ? '已复制'
    : copyStatus === 'failed'
      ? '复制失败，请手动选择'
      : '复制 JSON'

  return (
    <aside className="config-preview">
      <div className="config-preview-header">
        <div>
          <div className="config-preview-title">开发者：配置文件实时预览</div>
          <div className="config-preview-file">{fileName}</div>
        </div>
        <button type="button" onClick={copy} disabled={!json}>
          {copyLabel}
        </button>
      </div>

      {json ? (
        <pre className="config-preview-code">{json}</pre>
      ) : (
        <div className="config-preview-empty">
          画布尚未形成有效通道。
          <span>请连接视频流与模型/逻辑节点后查看完整配置。</span>
        </div>
      )}
    </aside>
  )
}
