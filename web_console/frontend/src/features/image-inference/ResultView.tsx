import { useEffect, useState } from 'react'
import { apiErrorMessage } from '../../api/http'
import { fetchImageResult, imageArtifactUrl, type ImageItem, type ImageJob, type ImageResult } from '../../api/imageInference'

export const statusText: Record<string, string> = {
  uploading: '准备图片', loading: '加载模型', running: '推理中', cancelling: '正在取消',
  completed: '已完成', completed_with_errors: '完成，部分失败', failed: '失败', cancelled: '已取消',
  pending: '等待', success: '成功', partial: '部分模型失败',
}

export default function ResultView({ job, item }: { job: ImageJob; item: ImageItem }) {
  const [result, setResult] = useState<ImageResult | null>(null)
  const [error, setError] = useState('')
  const [view, setView] = useState('all')
  useEffect(() => {
    let stale = false
    setResult(null); setError('')
    if (item.result) void fetchImageResult(job.id, item.index).then(value => {
      if (!stale) setResult(value)
    }).catch(error => { if (!stale) setError(apiErrorMessage(error)) })
    return () => { stale = true }
  }, [job.id, item.index, item.result])
  const model = result?.models.find(m => `model:${m.id}` === view)
  const file = model?.annotated ?? result?.annotated
  const objects = result?.results.filter(r => !model || r.model_id === model.id) ?? []
  const artifact = (name: string) => imageArtifactUrl(job.id, item.index, name)
  return <section className="image-result" aria-label="推理结果">
    <div className="image-result-title"><strong>{item.name}</strong><span className={`image-status ${item.status}`}>{statusText[item.status] ?? item.status}</span></div>
    {(item.error || error) && <div className="image-error" role="alert">{item.error || error}</div>}
    {!item.result && !item.error && <div className="image-empty">{item.status === 'pending' || item.status === 'running' ? '等待推理结果…' : '暂无结果'}</div>}
    {item.result && !result && !error && <div className="image-empty">正在读取结果…</div>}
    {result && <>
      {result.models.length > 1 && <div className="image-result-tools">
        <select aria-label="结果视图" value={model ? view : 'all'} onChange={e => setView(e.target.value)}>
          <option value="all">合并结果</option>
          {result.models.map(m => <option key={m.id} value={`model:${m.id}`} disabled={!m.succeeded}>{m.id} · {m.type}{!m.succeeded ? '（失败）' : ''}</option>)}
        </select>
      </div>}
      {result.models.some(m => !m.succeeded) && <div className="image-warning" role="alert">
        {result.models.filter(m => !m.succeeded).map(m => <p key={m.id}>{m.id}：{m.error || '推理失败'}</p>)}
      </div>}
      <div className="image-compare" aria-label="原图与推理图对照">
        <figure><figcaption>原图</figcaption><div className="image-stage" data-view="original">
          <img src={artifact(result.original)} alt={`${item.name} · 原图`} />
        </div></figure>
        <figure><figcaption title={model?.id}>{model ? `推理图 · ${model.id}` : '推理图'}</figcaption><div className="image-stage" data-view="inference">
          {file && <img src={artifact(file)} alt={`${item.name} · ${model?.id ?? '所有模型推理结果'}`} />}
        </div></figure>
      </div>
      <p className="image-result-meta">{result.source_width} × {result.source_height} · {result.elapsed_ms.toFixed(1)} ms</p>
      <div className="image-result-details" role="region" aria-label="详细结果">
      {item.source_path && <p className="image-note image-source-origin">{item.source_path}</p>}
      <div className="image-table-wrap"><table>
        <thead><tr><th>模型</th><th>输入尺寸</th><th>阈值 / NMS</th><th>预处理 / 推理 / 后处理</th><th>状态</th></tr></thead>
        <tbody>{result.models.map(m => {
          const settings = job.settings.models.find(s => s.id === m.id)
          return <tr key={m.id}>
            <td><strong>{m.id}</strong><br /><small>{m.type}</small></td><td>{m.input_width} × {m.input_height}</td>
            <td>{settings?.obj_thresh.toFixed(2)} / {settings?.nms_thresh.toFixed(2)}</td>
            <td>{m.preprocess_ms.toFixed(1)} / {m.infer_ms.toFixed(1)} / {m.postprocess_ms.toFixed(1)} ms</td>
            <td>{m.succeeded ? '成功' : <span className="image-error-text">{m.error || '失败'}</span>}
            </td>
          </tr>
        })}</tbody>
      </table></div>
      <p className="image-note">耗时不含模型加载和文件保存；坐标基于 {result.coordinate_width} × {result.coordinate_height} 画布。</p>
      <div className="image-table-wrap image-objects"><table>
        <thead><tr><th>模型 ID</th><th>类别</th><th>置信度</th><th>位置 x, y, w, h</th><th>附加结果</th></tr></thead>
        <tbody>{objects.map((r, i) => <tr key={i}>
          <td>{r.model_id}</td><td>{r.depth_kind ? '深度图' : r.label || `类别 ${r.class_id}`}</td>
          <td>{r.depth_kind ? '—' : r.score.toFixed(2)}</td>
          <td>{r.depth_kind ? '整帧' : [r.box.x, r.box.y, r.box.width, r.box.height].map(n => n.toFixed(1)).join(', ')}</td>
          <td>{r.obb_corners ? '旋转框' : r.keypoints ? `${r.pose_schema} · ${r.keypoints.length} 个关键点` : r.depth_kind ?? '—'}</td>
        </tr>)}</tbody>
      </table>{!objects.length && <p className="image-empty">未检测到符合阈值的目标</p>}</div>
      </div>
    </>}
  </section>
}
