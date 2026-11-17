import { useState } from 'react'
import { apiErrorMessage } from '../../api/http'
import type { UploadedImageAsset } from '../../api/imageInference'

type UploadState =
  | { phase: 'uploading'; name: string; progress: number }
  | { phase: 'success'; asset: UploadedImageAsset }
  | { phase: 'error'; name: string; message: string }

const filename = (path: string) => path.split('/').pop() || path

export default function ModelAssetInput({ title, value, options, extension, required, onChange, onUpload }: {
  title: string; value: string; options: string[]; extension: '.rknn' | '.txt'; required?: boolean
  onChange: (value: string) => void
  onUpload: (file: File, onProgress: (value: number) => void) => Promise<UploadedImageAsset>
}) {
  const [upload, setUpload] = useState<UploadState | null>(null)
  const [editingPath, setEditingPath] = useState(false)
  const uploaded = upload?.phase === 'success' && upload.asset.path === value ? upload.asset : null
  const paths = [...new Set([...options, ...(value ? [value] : [])])]
  const select = (path: string) => { setUpload(null); onChange(path) }
  const importFile = async (file: File) => {
    setUpload({ phase: 'uploading', name: file.name, progress: 0 })
    try {
      if (!file.name.toLowerCase().endsWith(extension)) throw new Error(`请选择 ${extension} 文件。`)
      if (!file.size) throw new Error('文件为空，请重新选择。')
      if (file.size > (extension === '.rknn' ? 1024 ** 3 : 4 * 1024 ** 2)) {
        throw new Error(extension === '.rknn' ? '单个模型不能超过 1 GiB。' : '标签文件不能超过 4 MiB。')
      }
      const asset = await onUpload(file, progress => setUpload(current =>
        current?.phase === 'uploading' ? { ...current, progress } : current))
      onChange(asset.path)
      setEditingPath(false)
      setUpload({ phase: 'success', asset })
    } catch (error) {
      setUpload({ phase: 'error', name: file.name, message: apiErrorMessage(error) })
    }
  }
  return <div className="image-model-asset">
    <div className="image-asset-heading">{extension === '.rknn' ? '模型文件' : required ? '标签' : '标签（可选）'}</div>
    <div className="image-asset-actions">
      {editingPath ? <input aria-label={title} autoFocus value={value} required={required} maxLength={4096}
        placeholder={`/userdata/...${extension}`} onChange={event => select(event.target.value)}
        onBlur={() => setEditingPath(false)} onKeyDown={event => {
          if (event.key === 'Enter' || event.key === 'Escape') { event.preventDefault(); setEditingPath(false) }
        }} /> : <select aria-label={title} value={value} required={required} title={value || title}
        onChange={event => {
          if (event.target.value === '__path__') setEditingPath(true)
          else select(event.target.value)
        }}>
        <option value="">选择{extension === '.rknn' ? '模型' : '标签'}</option>
        {paths.map(path => <option key={path} value={path}>{filename(path)}</option>)}
        <option value="__path__">输入板端路径…</option>
      </select>}
      <label className="image-asset-upload" title={extension === '.rknn' ? '上传 .rknn，最大 1 GiB' : '上传 .txt，最大 4 MiB'}><span>上传</span>
        <input aria-label={`上传${title}`} type="file" accept={extension} onChange={event => {
          const file = event.target.files?.[0]
          event.target.value = '' // Allow retrying the same file; feedback keeps its name visible.
          if (file) void importFile(file)
        }} />
      </label>
    </div>
    {upload?.phase === 'uploading' && <div className="image-asset-feedback" role="status">
      <span>{upload.name} · {upload.progress >= 100 ? '保存中…' : `${upload.progress}%`}</span>
      <progress aria-label={`${title}上传进度`} max={100} value={upload.progress} />
    </div>}
    {upload?.phase === 'error' && <div className="image-asset-feedback image-asset-error" role="alert">
      上传失败：{upload.message}
    </div>}
    {uploaded && <div className="image-asset-feedback image-asset-selected" role="status">
      {uploaded.renamed ? `已上传 · 同名文件已保留，另存为 ${uploaded.name}` : '✓ 已上传'}
    </div>}
  </div>
}
