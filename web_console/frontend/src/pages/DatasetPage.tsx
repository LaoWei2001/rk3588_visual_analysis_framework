import {useEffect, useRef, useState} from 'react'
import DatasetTaskEditor, {type DatasetDraft} from '../components/DatasetTaskEditor'
import {targetsFrom, validCondition, type Rule} from '../components/datasetRules'
import {apiErrorMessage, fetchApps, type AppInfo} from '../api/client'
import {fetchDataset, saveDataset, enableDataset, deleteDataset, downloadDatasetReceiver,
  type DatasetConfig, type DatasetStatus, type DatasetTask,
  type DatasetReceiverPlatform} from '../api/dataset'
import './DatasetPage.css'

const initial: DatasetConfig = {rules: [], interval_sec: 2, confirm_sec: 0, max_samples: 0, images_per_folder: 30,
  duration_hours: 0, jpeg_quality: 95, roi_name: '', roi_anchor: 'center', trigger_mode: 'periodic'}

export default function DatasetPage() {
  const [apps, setApps] = useState<AppInfo[]>([])
  const [app, setApp] = useState('')
  const [status, setStatus] = useState<DatasetStatus | null>(null)
  const [draft, setDraft] = useState<DatasetDraft | null>(null)
  const [busy, setBusy] = useState(false)
  const [downloading, setDownloading] = useState(false)
  const [error, setError] = useState('')
  const [loadError, setLoadError] = useState('')
  const [formError, setFormError] = useState('')
  const [notice, setNotice] = useState('')
  const [receiverPlatform, setReceiverPlatform] = useState<DatasetReceiverPlatform>(
    /Windows/i.test(navigator.userAgent) ? 'windows-amd64' : 'python')
  const actionPending = useRef(false)
  const statusRevision = useRef(0)
  const editorTrigger = useRef<HTMLButtonElement | null>(null)
  const editorPanel = useRef<HTMLElement>(null)
  const tasksPanel = useRef<HTMLElement>(null)
  const dirty = !!draft && JSON.stringify(draft.config) !== draft.original
  const discardDraft = () => !dirty || window.confirm('配置尚未保存，放弃本次修改？')
  const availableChannels = status?.channels.filter(channel => channel.eligible && !status.tasks.some(task => task.channel_id === channel.id)) ?? []
  const createReason = !app ? '请选择视觉程序。' : !status ? loadError ? '采集状态加载失败。' : '正在加载采集状态…'
    : availableChannels.length ? '' : status.channels.some(channel => channel.eligible)
      ? '所有可用通道都已有任务，可在任务卡片中编辑配置。' : '暂无可用的单模型检测通道。'

  useEffect(() => {
    let disposed = false
    fetchApps().then(items => {
      if (disposed) return
      setApps(items)
      setApp(items.find(item => item.status === 'running')?.name ?? items[0]?.name ?? '')
    }).catch(error => {if (!disposed) setError(apiErrorMessage(error))})
    return () => {disposed = true}
  }, [])
  useEffect(() => {
    setStatus(null); setDraft(null); setError(''); setLoadError(''); setFormError(''); setNotice('')
    if (!app) return
    let disposed = false, active = false
    const refresh = async () => {
      if (active || actionPending.current) return
      active = true
      const revision = statusRevision.current
      try {
        const next = await fetchDataset(app)
        if (!disposed && revision === statusRevision.current) {setStatus(next); setLoadError('')}
      } catch (error) {
        if (!disposed && revision === statusRevision.current) setLoadError(apiErrorMessage(error))
      } finally {active = false}
    }
    void refresh()
    const timer = window.setInterval(refresh, 2000)
    return () => {disposed = true; window.clearInterval(timer)}
  }, [app])
  useEffect(() => {
    if (!dirty) return
    const warn = (event: BeforeUnloadEvent) => {event.preventDefault(); event.returnValue = ''}
    window.addEventListener('beforeunload', warn)
    return () => window.removeEventListener('beforeunload', warn)
  }, [dirty])

  const configureDraft = (channelId: number, task?: DatasetTask) => {
    const source = status?.channels.find(channel => channel.id === channelId)
    const config = task?.config ?? {...initial, rules: [{id: 'rule_1', name: '采集条件', condition: {
      class: source?.labels[0] ?? '', min_confidence: Math.max(.3, source?.threshold ?? .3), count: {op: '>=', value: 1},
    }}]}
    setDraft({taskId: task?.id ?? null, channelId, config, original: JSON.stringify(config)})
    setError(''); setFormError(''); setNotice('')
  }
  const openCreate = (trigger: HTMLButtonElement) => {
    if (!availableChannels.length || !discardDraft()) return
    editorTrigger.current = trigger
    configureDraft(availableChannels[0].id)
  }
  const openEdit = (task: DatasetTask, trigger: HTMLButtonElement) => {
    if (draft?.taskId === task.id) {
      editorPanel.current?.scrollIntoView({behavior: 'smooth', block: 'start'})
      editorPanel.current?.querySelector('h2')?.focus({preventScroll: true})
      return
    }
    if (!discardDraft()) return
    editorTrigger.current = trigger
    configureDraft(task.channel_id, task)
  }
  const closeEditor = () => {
    setDraft(null); setFormError('')
    window.requestAnimationFrame(() => {
      if (editorTrigger.current?.isConnected && !editorTrigger.current.disabled) editorTrigger.current.focus()
      else tasksPanel.current?.querySelector('h2')?.focus()
    })
  }
  const act = async (operation: () => Promise<DatasetStatus>, message: string, reportError = setError) => {
    if (actionPending.current) return null
    actionPending.current = true
    ++statusRevision.current // Discard status reads started before this change.
    setBusy(true); setError(''); setFormError(''); setNotice('')
    try {const next = await operation(); setStatus(next); setLoadError(''); setNotice(message); return next}
    catch (error) {reportError(apiErrorMessage(error)); return null}
    finally {actionPending.current = false; setBusy(false)}
  }
  const save = async () => {
    if (!draft) return
    const rules = draft.config.rules
    if (!rules.length || !rules.every(rule => validCondition((rule as Rule)?.condition))) {
      setFormError('请填写完整的采集条件。'); return
    }
    const targets = (rules as Rule[]).flatMap(rule => targetsFrom(rule.condition))
    if (targets.some(target => !target.class.trim())) {setFormError('请填写所有条件中的类别名称。'); return}
    const source = status?.channels.find(channel => channel.id === draft.channelId)
    if (source?.labels.length && targets.some(target => !source.labels.includes(target.class))) {
      setFormError('类别名称必须与该通道的模型标签一致。'); return
    }
    const next = await act(() => saveDataset(app, draft.channelId, draft.config),
      draft.taskId ? '配置已保存。' : '任务已创建，点击“开始采集”开始。', setFormError)
    if (next) closeEditor()
  }
  const download = async () => {
    setDownloading(true); setError(''); setNotice('')
    try {
      const blob = await downloadDatasetReceiver(window.location.origin, receiverPlatform)
      const link = document.createElement('a'); link.href = URL.createObjectURL(blob)
      link.download = receiverPlatform === 'windows-amd64' ? 'dataset-receiver-windows-amd64.exe' : 'dataset-receiver.zip'
      link.click(); window.setTimeout(() => URL.revokeObjectURL(link.href), 30000)
      setNotice(receiverPlatform === 'windows-amd64'
        ? 'EXE 已下载，运行后填写设备地址并选择保存目录。'
        : '接收包已下载，解压后运行并选择保存目录。')
    } catch (error) {setError(apiErrorMessage(error))}
    finally {setDownloading(false)}
  }
  const stopOrStart = (task: DatasetTask) => void act(() => enableDataset(app, task.id, !task.enabled),
    task.enabled ? '任务已暂停，待传图片仍可继续接收。' : '任务已启动。')
  const remove = async (task: DatasetTask) => {
    const message = status?.engine_error
      ? '删除这个采集任务？连接恢复后会清理旧任务及未传图片，电脑上已保存的图片会保留。'
      : '删除这个采集任务？电脑上已保存的图片会保留。'
    if (!window.confirm(message)) return
    const next = await act(() => deleteDataset(app, task.id), '采集任务已删除。')
    if (next && draft?.taskId === task.id) closeEditor()
  }
  return <div className="dataset-page">
    <div className="dataset-page-heading"><h1>数据集采集</h1>
      <label>视觉程序<select aria-label="采集程序" value={app} disabled={busy} onChange={event => {
        if (discardDraft()) setApp(event.target.value)
      }}>
        {!apps.length && <option value="">没有可用程序</option>}
        {apps.map(item => <option key={item.name} value={item.name}>{item.name} · {item.status === 'running' ? '运行中' : '未运行'}</option>)}
      </select></label>
    </div>
    {(error || loadError) && <div className="dataset-banner error" role="alert">{error || loadError}</div>}
    {notice && <div className="dataset-banner success" role="status">{notice}</div>}
    <section className="dataset-receiver-panel">
      <div><h2>电脑接收端 <span className={status?.receiver.online?'dataset-online':'dataset-offline'}>{status ? status.receiver.online ? '在线' : '未连接' : '状态未知'}</span></h2>
        {status?.receiver.directory && <p>保存目录：{status.receiver.directory}</p>}
        {status?.receiver.error && <p className="dataset-receiver-error">{status.receiver.error}</p>}
      </div>
      <div className="dataset-receiver-setup">
        <h3>接收程序下载与连接配置</h3>
        <div className="dataset-receiver-actions"><label>设备地址<input aria-label="设备地址" value={window.location.origin} readOnly onFocus={e => e.currentTarget.select()} /></label>
        <label>电脑系统<select aria-label="接收程序类型" value={receiverPlatform} disabled={downloading}
          onChange={e => setReceiverPlatform(e.target.value as DatasetReceiverPlatform)}>
          <option value="windows-amd64">Windows 10/11 64 位（无需 Python）</option>
          <option value="python">Linux / macOS（Python 3.9+）</option>
        </select></label>
        <button disabled={downloading} onClick={download}>{downloading ? '正在下载…' : receiverPlatform==='windows-amd64'?'下载通用 Windows EXE':'下载 Python 接收包'}</button>
        <small>运行后填写设备地址并选择保存目录。采集时保持接收程序运行，新增任务无需重新下载。</small>
      </div>
      </div>
    </section>
    {status?.engine_error && status.tasks.length > 0 && <div className="dataset-banner">{status.engine_error}</div>}
    <div className="dataset-page-sections">
      <section className="dataset-tasks-panel" ref={tasksPanel}>
        <div className="dataset-section-heading"><h2 tabIndex={-1}>采集任务 <span>{status ? `${status.tasks.length} 个` : '—'}</span></h2>
          <button disabled={busy || !!createReason || draft?.taskId === null}
            title={draft?.taskId === null ? '请先完成或取消当前新建任务。' : createReason || undefined}
            aria-controls="dataset-task-editor" aria-expanded={draft?.taskId === null}
            onClick={event => openCreate(event.currentTarget)}>＋ 新建采集任务</button>
        </div>
        {!status ? <div className="dataset-empty">{!app ? '请选择视觉程序。' : loadError ? '暂时无法读取采集任务，正在重试。' : '正在加载采集任务…'}</div>
          : !status.tasks.length && <div className="dataset-empty">{createReason || '暂无任务，点击“新建采集任务”开始配置。'}</div>}
        {status && status.tasks.length > 0 && createReason && <p>{createReason}</p>}
        <div className="dataset-task-grid">
          {status?.tasks.map(task => {
            const expired = task.deadline > 0 && task.deadline * 1000 <= Date.now()
            const full = task.config.max_samples > 0 && task.saved >= task.config.max_samples
            const startReason = task.enabled ? '' : status.engine_error || (expired ? '运行时长已结束，请重新创建任务。' : full ? '已达到图片上限，可编辑配置提高上限。' : '')
            const deleteReason = status.engine_error ? '' : task.enabled ? '请先暂停采集。' : task.pending > 0 ? '待传图片接收完毕后可删除。' : ''
            return <article className="dataset-task" key={task.id}>
              <div className="dataset-task-heading"><h3>通道 {task.channel_id}</h3>
                <span className={task.enabled && !expired && !full && !status.engine_error ? 'dataset-online' : 'dataset-offline'}>{task.status}</span>
              </div>
              <div className="dataset-task-counters"><div title={task.files_updated_at ? `统计于 ${new Date(task.files_updated_at * 1000).toLocaleString()}` : undefined}><strong>{task.current_files ?? '—'}</strong><span>当前存图</span></div><div><strong>{task.pending}</strong><span>等待接收</span></div><div><strong>{task.skipped}</strong><span>本次跳过</span></div></div>
              {task.files_error ? <p className="dataset-receiver-error">存图统计失败：{task.files_error}</p>
                : task.current_files == null ? <p>等待接收端统计所选目录。</p>
                : !status.receiver.online && <p>接收端未连接，存图数量为上次统计。</p>}
              <p>累计保存 {task.saved} 张 · 间隔 {task.config.interval_sec} 秒 · 每文件夹 {task.config.images_per_folder} 张 · {task.config.max_samples ? `累计上限 ${task.config.max_samples} 张` : '不限张数'}</p>
              {task.deadline > 0 && <p>结束时间：{new Date(task.deadline * 1000).toLocaleString()}</p>}
              <div className="dataset-task-actions">
                <button disabled={busy || !!startReason} title={startReason || undefined} onClick={() => stopOrStart(task)}>{task.enabled ? '暂停采集' : '开始采集'}</button>
                <button className="secondary" disabled={busy} aria-controls="dataset-task-editor" aria-expanded={draft?.taskId === task.id}
                  onClick={event => openEdit(task, event.currentTarget)}>{task.enabled ? '查看配置' : '编辑配置'}</button>
                <button className="secondary" disabled={busy || !!deleteReason} title={deleteReason || undefined} onClick={() => void remove(task)}>删除任务</button>
              </div>
              {!task.enabled && (task.pending > 0 || expired || full) && <p>{task.pending > 0 ? deleteReason : startReason}</p>}
            </article>
          })}
        </div>
      </section>
      {draft && status && <DatasetTaskEditor key={`${app}:${draft.taskId ?? 'new'}`} draft={draft} status={status} busy={busy} error={formError} panelRef={editorPanel}
        onChange={values => {setDraft(current => current ? {...current, config: {...current.config, ...values}} : null); setFormError('')}}
        onChannelChange={channelId => {if (discardDraft()) configureDraft(channelId)}}
        onCancel={() => {if (discardDraft()) closeEditor()}} onSave={() => void save()}
        onPause={task => void act(() => enableDataset(app, task.id, false), '任务已暂停，可以修改配置。', setFormError)} />}
    </div>
  </div>
}
