import { useEffect, useRef, useState } from 'react'
import { apiErrorMessage } from '../../api/http'
import { inspectDeviceImages, type DeviceImages } from '../../api/imageInference'

export interface SourceChoice {
  kind: 'upload' | 'device'
  path: string
  recursive: boolean
}

export default function SourcePicker({ source, files, availableBytes, disabled, onChange, onFiles, onClear }: {
  source: SourceChoice; files: File[]; availableBytes?: number; disabled: boolean
  onChange: (source: SourceChoice) => void; onFiles: (files: FileList | null) => void; onClear: () => void
}) {
  const [preview, setPreview] = useState<DeviceImages | null>(null)
  const [error, setError] = useState('')
  const [checking, setChecking] = useState(false)
  const requestVersion = useRef(0)
  const [urls, setUrls] = useState<string[]>([])
  useEffect(() => {
    const next = files.slice(0, 60).map(file => URL.createObjectURL(file))
    setUrls(next)
    return () => next.forEach(url => URL.revokeObjectURL(url))
  }, [files])
  useEffect(() => {
    requestVersion.current++
    setPreview(null); setError(''); setChecking(false)
    return () => { requestVersion.current++ }
  }, [source.kind, source.path, source.recursive])
  const inspect = async () => {
    const version = ++requestVersion.current
    setChecking(true); setPreview(null); setError('')
    try {
      const value = await inspectDeviceImages(source.path.trim(), source.recursive)
      if (version === requestVersion.current) setPreview(value)
    } catch (error) {
      if (version === requestVersion.current) setError(apiErrorMessage(error))
    } finally {
      if (version === requestVersion.current) setChecking(false)
    }
  }
  return <section className="image-source-picker" aria-label="图片来源">
    <div className="image-source-heading"><h2>待推理图片</h2>
      <select aria-label="选择图片来源" value={source.kind} disabled={disabled}
        onChange={event => onChange({ ...source, kind: event.target.value as SourceChoice['kind'] })}>
        <option value="upload">电脑上传</option><option value="device">板端文件</option>
      </select>
    </div>
    {availableBytes !== undefined && <p className="image-note">可用 {(availableBytes / 1024 ** 2).toFixed(1)} MB · 已预留 100 MB</p>}
    {source.kind === 'upload' ? <>
      <div className="image-upload-row"><label className="image-upload" title="JPG / PNG / BMP / WebP，数量不限，需保留超过 100 MB 空间">
        <span>{files.length ? `已选 ${files.length} 张 · 重新选择` : '＋ 选择图片'}</span><input aria-label="选择图片" type="file" multiple
        accept=".jpg,.jpeg,.png,.bmp,.webp" disabled={disabled} onChange={e => onFiles(e.target.files)} /></label>
        {!!files.length && <button type="button" className="image-clear" aria-label="清空图片" title="清空图片" disabled={disabled} onClick={onClear}>×</button>}
      </div>
      {!!files.length && <>
        <div className="image-thumbnails">{files.slice(0, 60).map((file, i) => <img key={`${i}-${file.name}`} src={urls[i]}
          alt={file.name} title={file.name} loading="lazy" decoding="async" />)}</div>
      </>}
    </> : <div className="image-device-source">
      <label>路径<input aria-label="板端图片或文件夹路径" value={source.path} required
        placeholder="/userdata/pictures 或 /userdata/test.jpg" disabled={disabled} maxLength={4096}
        onChange={e => onChange({ ...source, path: e.target.value })} /></label>
      <label className="image-device-recursive"><input type="checkbox" checked={source.recursive} disabled={disabled}
        onChange={e => onChange({ ...source, recursive: e.target.checked })} />包括子文件夹</label>
      <button type="button" disabled={disabled || checking || !source.path.trim()}
        aria-label="读取图片清单" className="image-text-button" onClick={() => void inspect()}>{checking ? '读取中…' : '预览图片'}</button>
      {error && <div className="image-error" role="alert">{error}</div>}
      {preview && <details className="image-device-preview">
        <summary>找到 {preview.total} 张图片</summary>
        <ul>{preview.images.map(image => <li key={image.name} title={image.path}>{image.name}</li>)}</ul>
      </details>}
    </div>}
  </section>
}
