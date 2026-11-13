import {useEffect, useRef, useState} from 'react'
import {Link} from 'react-router-dom'
import DatasetTaskEditor from '../components/DatasetTaskEditor'
import DatasetTaskLogic from '../components/DatasetTaskLogic'
import {targetsFrom, validCondition, type Rule} from '../components/datasetRules'
import {apiErrorMessage, fetchApps, type AppInfo} from '../api/client'
import {fetchDataset, saveDataset, enableDataset, deleteDataset, downloadDatasetReceiver} from '../api/dataset'
import type {DatasetConfig, DatasetDraft, DatasetStatus, DatasetTask, DatasetReceiverPlatform} from '../models/dataset'
import './DatasetPage.css'
import {useDatasetStore} from '../store/datasetStore'
import {configSignature, useAutoSync} from '../hooks/useAutoSync'
import ConfigSyncNotice from '../components/ConfigSyncNotice'

const initial: DatasetConfig = {rules: [], interval_sec: 2, confirm_sec: 0, max_samples: 0, images_per_folder: 30,
  duration_hours: 0, roi_name: '', roi_anchor: 'center', trigger_mode: 'periodic'}
const originalSignature = (text: string) => { try { return configSignature(JSON.parse(text)) } catch { return text } }

export default function DatasetPage() {
  const [apps, setApps] = useState<AppInfo[]>([])
  const app = useDatasetStore(state => state.app)
  const setApp = useDatasetStore(state => state.setApp)
  const [status, setStatus] = useState<DatasetStatus | null>(null)
  const draft = useDatasetStore(state => state.drafts[app] ?? null)
  const updateDraft = useDatasetStore(state => state.setDraft)
  const setDraft = (update: DatasetDraft | null | ((current: DatasetDraft | null) => DatasetDraft | null)) => updateDraft(app, update)
  const [busy, setBusy] = useState(false)
  const [downloading, setDownloading] = useState(false)
  const [receiverSetup, setReceiverSetup] = useState(false)
  const [error, setError] = useState('')
  const [loadError, setLoadError] = useState('')
  const [formError, setFormError] = useState('')
  const [notice, setNotice] = useState('')
  const receiverPlatform = useDatasetStore(state => state.receiverPlatform)
  const setReceiverPlatform = useDatasetStore(state => state.setReceiverPlatform)
  const actionPending = useRef(false)
  const statusRevision = useRef(0)
  const editorTrigger = useRef<HTMLButtonElement | null>(null)
  const editorPanel = useRef<HTMLElement>(null)
  const editorFocusRequested = useRef(false)
  const tasksPanel = useRef<HTMLElement>(null)
  const selectedApp = apps.find(item => item.name === app)
  const runningTasks = status?.tasks.filter(task => task.enabled && !status.engine_error && !(task.deadline > 0 && task.deadline * 1000 <= Date.now()) && !(task.config.max_samples > 0 && task.saved >= task.config.max_samples)).length ?? 0
  const dirty = !!draft && JSON.stringify(draft.config) !== draft.original
  const selectedTask = draft?.taskId ? status?.tasks.find(task => task.id === draft.taskId) : null
  const draftConflict = !!draft?.taskId && !!status && (!selectedTask || configSignature(selectedTask.config) !== originalSignature(draft.original))
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
      const selected = useDatasetStore.getState().app
      if (!items.some(item => item.name === selected)) {
        setApp(items.find(item => item.status === 'running')?.name ?? items[0]?.name ?? '')
      }
    }).catch(error => {if (!disposed) setError(`读取程序列表失败：${apiErrorMessage(error)}`)})
    return () => {disposed = true}
  }, [])
  const appsSyncError = useAutoSync(fetchApps, items => {
    setApps(items)
    const selected = useDatasetStore.getState().app
    if (!items.some(item => item.name === selected)) setApp(items.find(item => item.status === 'running')?.name ?? items[0]?.name ?? '')
    setError(current => current.startsWith('读取程序列表失败：') ? '' : current)
  }, { immediate: false })
  useEffect(() => {
    editorFocusRequested.current = false
    setStatus(null); setError(''); setLoadError(''); setFormError(''); setNotice('')
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
    const timer = window.setInterval(refresh, 1000)
    return () => {disposed = true; window.clearInterval(timer)}
  }, [app])
  useEffect(() => {
    if (!status) return
    setDraft(current => {
      if (!current?.taskId || JSON.stringify(current.config) !== current.original) return current
      const task = status.tasks.find(item => item.id === current.taskId)
      if (!task) return null
      if (configSignature(task.config) === configSignature(current.config)) return current
      return {...current, config: task.config, original: JSON.stringify(task.config)}
    })
  }, [status])
  useEffect(() => {
    // Restoring a draft on app switch or page entry must not move the viewport.
    if (!editorFocusRequested.current || !draft || !status) return
    editorFocusRequested.current = false
    editorPanel.current?.scrollIntoView({behavior: 'smooth', block: 'start'})
    editorPanel.current?.querySelector('h2')?.focus({preventScroll: true})
  }, [draft, status])
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
    editorFocusRequested.current = true
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
    editorFocusRequested.current = true
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
    if (draftConflict || loadError) {setFormError('底层任务配置已变化或读取失败，请先重新同步。'); return}
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
    const next = await act(async () => {
      if (draft.taskId) {
        const latest = await fetchDataset(app)
        setStatus(latest)
        const task = latest.tasks.find(item => item.id === draft.taskId)
        if (!task || configSignature(task.config) !== originalSignature(draft.original)) throw new Error('底层采集任务已变化，请重新同步后再保存。')
      }
      return saveDataset(app, draft.channelId, draft.config)
    },
      draft.taskId ? '配置已保存。' : '任务已创建。运行视觉程序和电脑接收端后，可按 F8 手动抓图；需要自动采集时点击“开始采集”。', setFormError)
    // A save can finish after navigating away. Do not discard a newer draft.
    if (next && useDatasetStore.getState().drafts[app] === draft) closeEditor()
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
  const receiverBinding = status?.receiver.control_task
  const captureTask = receiverBinding?.app === app ? status?.tasks.find(task => task.id === receiverBinding.task_id) : undefined
  const remove = async (task: DatasetTask) => {
    const message = status?.engine_error
      ? '删除这个采集任务？连接恢复后会清理旧任务及未传图片，电脑上已保存的图片会保留。'
      : '删除这个采集任务？电脑上已保存的图片会保留。'
    if (!window.confirm(message)) return
    const next = await act(() => deleteDataset(app, task.id), '采集任务已删除。')
    if (next && draft?.taskId === task.id) closeEditor()
  }
  return <div className="dataset-page">
    <div className="dataset-page-heading"><div><h1>数据集采集</h1>
      <p>按识别条件自动抓取原图，保存到你的电脑。</p></div>
      <span className={runningTasks ? 'dataset-online' : 'dataset-offline'}>{!status || loadError ? '正在确认采集状态' : runningTasks ? `${runningTasks} 个任务采集中` : '当前没有采集中的任务'}</span>
    </div>
    {(error || loadError) && <div className="dataset-banner error" role="alert">{error || loadError}</div>}
    {notice && <div className="dataset-banner success" role="status">{notice}</div>}
    <div className="dataset-preparation">
      <section className="dataset-source-panel" aria-labelledby="dataset-source-title">
        <h2 id="dataset-source-title"><span className="dataset-step">1</span> 选择采集来源</h2>
        <div className="dataset-app-picker">
          <label>选择要采集的视觉程序<select aria-label="采集程序" aria-describedby="dataset-app-help" value={app} disabled={busy} onChange={event => setApp(event.target.value)}>
            {!apps.length && <option value="">没有可用程序</option>}
            {apps.map(item => <option key={item.name} value={item.name}>{item.name} · {item.status === 'running' ? '运行中' : '未运行'}</option>)}
          </select></label>
          <p id="dataset-app-help">先选择程序，再新建采集任务。下方任务、通道和类别均属于所选程序。</p>
        </div>
        {selectedApp && <p className="dataset-source-config">通道配置：<strong>{selectedApp.active_config}</strong></p>}
        <div className="dataset-source-channels">
          {status?.channels.map(channel => <div key={channel.id}>
            <strong>视频通道 {channel.id}</strong>
            <span>{channel.labels.join(' · ') || '未读取到类别表'}</span>
            {!channel.eligible && <small>暂不支持采集，需要单个检测模型</small>}
          </div>)}
          {status && !status.channels.length && <p>当前配置没有可用的视频通道。</p>}
        </div>
        {selectedApp?.status === 'stopped' && <p className="dataset-source-hint">程序未运行，可以先配置任务。<Link to="/">前往程序管理启动 →</Link></p>}
      </section>

    <section className="dataset-receiver-panel">
      <div><h2><span className="dataset-step">2</span> 连接保存图片的电脑 <span className={status?.receiver.online?'dataset-online':'dataset-offline'}>{status ? status.receiver.online ? '在线' : '未连接' : '状态未知'}</span></h2>
        {status?.receiver.directory && <p>保存目录：{status.receiver.directory}</p>}
        {status?.receiver.error && <p className="dataset-receiver-error">{status.receiver.error}</p>}
      </div>
      <p>{status?.receiver.online ? '电脑已连接，采集的图片会自动保存到上述目录。' : '在接收图片的电脑上下载并运行接收端，选择保存目录后，连接状态会自动更新。'}</p>
      {status?.receiver.online && <button className="secondary" aria-expanded={receiverSetup} onClick={() => setReceiverSetup(!receiverSetup)}>{receiverSetup ? '收起连接设置' : '更换电脑 / 下载接收端'}</button>}
      {(!status?.receiver.online || receiverSetup) && <div className="dataset-receiver-setup">
        <div className="dataset-receiver-actions"><label>设备地址<input aria-label="设备地址" value={window.location.origin} readOnly onFocus={e => e.currentTarget.select()} /></label>
        <label>电脑系统<select aria-label="接收程序类型" value={receiverPlatform} disabled={downloading}
          onChange={e => setReceiverPlatform(e.target.value as DatasetReceiverPlatform)}>
          <option value="windows-amd64">Windows 10/11 64 位（无需 Python）</option>
          <option value="python">Linux / macOS（Python 3.9+）</option>
        </select></label>
        <button disabled={downloading} onClick={download}>{downloading ? '正在下载…' : receiverPlatform==='windows-amd64'?'下载通用 Windows EXE':'下载 Python 接收包'}</button>
        <small>填写设备地址和保存目录，采集时保持接收端运行。</small>
      </div>
      </div>}
    </section>
    </div>
    {status?.engine_error && <div className="dataset-banner">所选程序暂不可采集，可以先创建或编辑任务。<details><summary>查看原因</summary>{status.engine_error}</details></div>}
    <div className="dataset-page-sections">
      <section className="dataset-tasks-panel" ref={tasksPanel}>
        <div className="dataset-section-heading"><h2 tabIndex={-1}><span className="dataset-step">3</span> 采集任务 <span>{status ? `${status.tasks.length} 个` : '—'}</span></h2>
          <button disabled={busy || !!createReason || draft?.taskId === null}
            title={draft?.taskId === null ? '请先完成或取消当前新建任务。' : createReason || undefined}
            aria-controls="dataset-task-editor" aria-expanded={draft?.taskId === null}
            onClick={event => openCreate(event.currentTarget)}>＋ 新建采集任务</button>
        </div>
        <div className="dataset-quick-controls">
          <strong>电脑快捷键</strong>
          <p>{!status?.receiver.online ? '运行电脑接收程序后，快捷键默认开启。'
            : !status.receiver.hotkeys_available ? '请运行新版 Windows 接收程序，确认快捷键注册成功。'
            : receiverBinding ? `当前操作：${receiverBinding.app} / 视频通道 ${receiverBinding.channel_id}。`
            : '快捷键已开启，等待可用任务；多个任务时在电脑上按 F7 选择。'}
            F8 抓一张，F9 暂停／恢复自动采集，F7 切换任务。只有一个可用任务时自动选中，无需网页确认。</p>
          <p>手动抓图跳过抓图条件，保存设备收到指令后的下一张解码原图；自动采集暂停时也可使用。播放器画面可能有延迟，抓取结果不一定是屏幕上当时显示的那一帧。</p>
          {captureTask?.manual_state && <p role="status">{captureTask.manual_state === 'waiting' ? '已发送请求，等待新视频帧…' : captureTask.manual_state === 'encoding' ? '已取到原图，正在编码…' : captureTask.manual_state === 'captured' ? `已抓取第 ${captureTask.manual_frame} 帧（设备收到请求到取帧 ${captureTask.manual_wait_ms} 毫秒）` : `手动抓图失败：${captureTask.manual_error}`}</p>}
        </div>
        <p className="dataset-task-help">{app ? `正在管理 ${app} 的采集任务。` : '先选择上方的视觉程序。'}手动抓图使用电脑 F8；需要按条件自动采集时，点击任务上的“开始采集”。</p>
      <ConfigSyncNotice error={appsSyncError} conflict={draftConflict} onReload={() => {
        if (selectedTask) configureDraft(selectedTask.channel_id, selectedTask)
        else closeEditor()
      }} />
      {draft && status && <DatasetTaskEditor key={`${app}:${draft.taskId ?? 'new'}`} appName={app} draft={draft} status={status} busy={busy || draftConflict || !!loadError} error={formError} panelRef={editorPanel}
        onChange={values => {setDraft(current => current ? {...current, config: {...current.config, ...values}} : null); setFormError('')}}
        onChannelChange={channelId => {if (discardDraft()) configureDraft(channelId)}}
        onCancel={() => {if (discardDraft()) closeEditor()}} onSave={() => void save()} onValidationError={setFormError}
        onPause={task => void act(() => enableDataset(app, task.id, false), '任务已暂停，可以修改配置。', setFormError)} />}
        {!status ? <div className="dataset-empty">{!app ? '请选择视觉程序。' : loadError ? '暂时无法读取采集任务，正在重试。' : '正在加载采集任务…'}</div>
          : !status.tasks.length && !draft && <div className="dataset-empty">{createReason || '暂无任务，点击“新建采集任务”开始配置。'}</div>}
        {status && status.tasks.length > 0 && createReason && <p>{createReason}</p>}
        <div className="dataset-task-grid">
          {status?.tasks.map(task => {
            const expired = task.deadline > 0 && task.deadline * 1000 <= Date.now()
            const full = task.config.max_samples > 0 && task.saved >= task.config.max_samples
            const startReason = task.enabled ? '' : status.engine_error || (expired ? '运行时长已结束，请重新创建任务。' : full ? '已达到图片上限，可编辑配置提高上限。' : '')
            const deleteReason = status.engine_error ? '' : task.enabled ? '请先暂停采集。' : task.pending > 0 ? '待传图片接收完毕后可删除。' : ''
            return <article className="dataset-task" key={task.id}>
              <div className="dataset-task-heading"><h3>视频通道 {task.channel_id}</h3>
                <span className={task.enabled && !expired && !full && !status.engine_error ? 'dataset-online' : 'dataset-offline'}>{task.status}</span>
              </div>
              <div className="dataset-task-counters"><div title={task.files_updated_at ? `统计于 ${new Date(task.files_updated_at * 1000).toLocaleString()}` : undefined}><strong>{task.current_files ?? '—'}</strong><span>当前存图</span></div><div><strong>{task.pending}</strong><span>等待传到电脑</span></div><div><strong>{task.skipped}</strong><span>本次跳过</span></div></div>
              {task.files_error ? <p className="dataset-receiver-error">存图统计失败：{task.files_error}</p>
                : task.current_files == null ? <p>等待接收端统计所选目录。</p>
                : !status.receiver.online && <p>接收端未连接，存图数量为上次统计。</p>}
              <p>累计保存 {task.saved} 张 · 间隔 {task.config.interval_sec} 秒 · 每文件夹 {task.config.images_per_folder} 张 · {task.config.max_samples ? `累计上限 ${task.config.max_samples} 张` : '不限张数'}</p>
              {task.deadline > 0 && <p>结束时间：{new Date(task.deadline * 1000).toLocaleString()}</p>}
              <div className="dataset-task-actions">
                <button disabled={busy || !!startReason} title={startReason || undefined} onClick={() => stopOrStart(task)}>{task.enabled ? '暂停采集' : '开始采集'}</button>
                <button className="secondary" disabled={busy} aria-controls="dataset-task-editor" aria-expanded={draft?.taskId === task.id}
                  onClick={event => openEdit(task, event.currentTarget)}>{task.enabled ? '查看配置' : '编辑配置'}</button>
                <button className="secondary dataset-delete" disabled={busy || !!deleteReason} title={deleteReason || undefined} onClick={() => void remove(task)}>删除任务</button>
              </div>
              {!task.enabled && startReason && <p className="dataset-task-hint">暂不能开始：{startReason}</p>}
              {!task.enabled && task.pending > 0 && !startReason && <p>{deleteReason}</p>}
              <DatasetTaskLogic config={task.config} />
            </article>
          })}
        </div>
      </section>

    </div>
  </div>
}
