import { useEffect, useRef, useState } from 'react'
import { fetchDeviceFolders, type DeviceFolders } from '../../api/imageInference'
import { apiErrorMessage } from '../../api/http'

export default function DeviceFolderPicker({ initialPath, onChoose, onCancel }: {
  initialPath: string; onChoose: (path: string) => void; onCancel: () => void
}) {
  const dialogRef = useRef<HTMLDialogElement>(null)
  const [path, setPath] = useState(initialPath.startsWith('/') ? initialPath : '/')
  const [listing, setListing] = useState<DeviceFolders | null>(null)
  const [error, setError] = useState('')
  const [retry, setRetry] = useState(0)

  useEffect(() => {
    const dialog = dialogRef.current!
    dialog.showModal()
    return () => dialog.close()
  }, [])

  useEffect(() => {
    const controller = new AbortController()
    setListing(null); setError('')
    void fetchDeviceFolders(path, controller.signal).then(result => {
      if (!controller.signal.aborted) setListing(result)
    }).catch(error => { if (!controller.signal.aborted) setError(apiErrorMessage(error)) })
    return () => controller.abort()
  }, [path, retry])

  return <dialog ref={dialogRef} className="image-folder-dialog" aria-labelledby="image-folder-heading"
    onCancel={event => { event.preventDefault(); onCancel() }}>
    <h2 id="image-folder-heading">选择板端文件夹</h2>
    <p className="image-note">打开存放图片的目录，点击「使用此文件夹」即可查看待推理图片数量。</p>
    <div className="image-folder-location">
      <button type="button" disabled={listing ? !listing.parent : !error || path === '/'}
        onClick={() => setPath(listing?.parent ?? '/')}>{error ? '返回根目录' : '上一级'}</button>
      <span>{listing?.path ?? path}</span>
    </div>
    {error ? <div className="image-error" role="alert">{error}
      <button type="button" onClick={() => setRetry(value => value + 1)}>重试</button></div>
      : !listing ? <p role="status">正在读取文件夹…</p>
      : <div className="image-folder-list">
        {listing.folders.map(name => <button type="button" key={name}
          onClick={() => setPath(`${listing.path === '/' ? '' : listing.path}/${name}`)}>
          {name}<span aria-hidden="true">›</span>
        </button>)}
        {!listing.folders.length && <p className="image-note">没有下一级文件夹，可以选择当前目录。</p>}
      </div>}
    <footer>
      <button type="button" onClick={onCancel}>取消</button>
      <button type="button" disabled={!listing} onClick={() => listing && onChoose(listing.path)}>使用此文件夹</button>
    </footer>
  </dialog>
}
