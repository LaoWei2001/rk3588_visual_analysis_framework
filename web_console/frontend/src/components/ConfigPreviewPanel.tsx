import { useState } from 'react'
import './ConfigPreviewPanel.css'

interface Props {
  fileName: string
  json: string | null
}

function fallbackCopy(text: string): boolean {
  const textarea = document.createElement('textarea')
  textarea.value = text
  textarea.setAttribute('readonly', '')
  textarea.style.position = 'fixed'
  textarea.style.left = '-9999px'
  textarea.style.top = '0'
  document.body.appendChild(textarea)
  try {
    textarea.focus()
    textarea.select()
    textarea.setSelectionRange(0, textarea.value.length)
    return document.execCommand('copy')
  } catch {
    return false
  } finally {
    document.body.removeChild(textarea)
  }
}

async function copyText(text: string): Promise<boolean> {
  if (navigator.clipboard && window.isSecureContext) {
    try {
      await navigator.clipboard.writeText(text)
      return true
    } catch {
      // Clipboard API 被拒绝时，继续尝试兼容普通 HTTP 的回退方案。
    }
  }
  return fallbackCopy(text)
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
          <div className="config-preview-title">配置文件实时预览</div>
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
