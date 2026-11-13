import type {RefObject} from 'react'
import type {DatasetConfig, DatasetDraft, DatasetStatus, DatasetTask} from '../models/dataset'
import DatasetRuleBuilder from './DatasetRuleBuilder'
import NumberField from './NumberField'
import {summary, validCondition, type Rule} from './datasetRules'

interface Props {
  appName: string
  draft: DatasetDraft
  status: DatasetStatus
  busy: boolean
  error: string
  panelRef: RefObject<HTMLElement>
  onChange: (values: Partial<DatasetConfig>) => void
  onChannelChange: (channel: number) => void
  onCancel: () => void
  onSave: () => void
  onValidationError: (message: string) => void
  onPause: (task: DatasetTask) => void
}

export default function DatasetTaskEditor({appName, draft, status, busy, error, panelRef: panel, onChange, onChannelChange, onCancel, onSave, onValidationError, onPause}: Props) {
  const creating = draft.taskId === null
  const task = status.tasks.find(task => task.id === draft.taskId)
  const source = status.channels.find(channel => channel.id === draft.channelId)
  const unavailable = !creating && !task
  const readOnly = busy || !!task?.enabled || unavailable
  const conflict = creating && status.tasks.some(task => task.channel_id === draft.channelId)
  const saveReason = unavailable ? '该任务已删除，请关闭配置。'
    : conflict ? '该通道已有采集任务，请取消新建并编辑已有任务。'
    : task && !task.enabled && task.pending > 0 ? '待传图片接收完毕后可保存修改。'
    : !source?.eligible ? '该通道当前不可用于采集。' : ''
  const config = draft.config
  return <section className="dataset-config-panel" id="dataset-task-editor" ref={panel}>
    <form onSubmit={event => {event.preventDefault(); if (!readOnly && !saveReason) onSave()}}
      onInvalidCapture={event => {
        const field = event.target
        if (!(field instanceof HTMLInputElement || field instanceof HTMLSelectElement)) return
        if (event.currentTarget.querySelector('input:invalid, select:invalid') !== field) return
        // 折叠的高级设置也参加原生表单校验；展开后浏览器才能显示提示并定位输入框。
        for (let parent = field.parentElement; parent && parent !== event.currentTarget; parent = parent.parentElement) {
          if (parent instanceof HTMLDetailsElement) parent.open = true
        }
        const label = [...(field.closest('label')?.childNodes ?? [])]
          .filter(node => node.nodeType === Node.TEXT_NODE).map(node => node.textContent).join('').trim()
        onValidationError(`请检查${label ? `“${label}”` : '输入内容'}：${field.validationMessage}`)
      }}>
      <div className="dataset-section-heading">
        <h2 tabIndex={-1}>{creating ? '新建采集任务' : `${task?.enabled ? '查看' : '编辑'}采集任务 · 通道 ${draft.channelId}`}</h2>
        <button type="button" className="secondary" disabled={busy} onClick={onCancel}>{task?.enabled || unavailable ? '关闭' : '取消'}</button>
      </div>
      {saveReason && <p className="dataset-editor-message">{saveReason}</p>}
      <p className="dataset-editor-message">采集程序：<strong>{appName}</strong> · 草稿自动保留，{creating ? '创建后手动开始采集' : '保存后生效'}。</p>
      <div className="dataset-channel-picker">
        <label>采集哪个视频通道<select aria-label="采集哪个视频通道" value={draft.channelId} disabled={busy || !creating} onChange={event => onChannelChange(Number(event.target.value))}>
          {!source && <option value={draft.channelId}>视频通道 {draft.channelId}</option>}
          {status.channels.map(channel => {
            const occupied = status.tasks.some(task => task.channel_id === channel.id)
            return <option key={channel.id} value={channel.id} disabled={!channel.eligible || (creating && occupied)}>
              视频通道 {channel.id}
            </option>
          })}
        </select></label>
        <div className="dataset-channel-labels"><span>此通道可用类别</span>
          <div className="dataset-label-chips">{source?.labels.length ? source.labels.map(label => <span key={label}>{label}</span>) : '未读取到类别表'}</div>
          <small>一个视频通道对应一路视频分析，下面的条件只使用此通道的识别结果。</small>
        </div>
      </div>
      <fieldset className="dataset-editor-grid" disabled={readOnly || conflict}>
        <div className="dataset-editor-conditions"><h3>什么情况下保存图片？</h3>
          <p>从下拉菜单中选择此视频通道的模型类别。</p>
          <DatasetRuleBuilder labels={source?.labels} value={config.rules} onChange={rules => onChange({rules: rules as unknown[]})} />
        </div>
        <div className="dataset-editor-saving"><h3>保存频率与数量</h3>
          <p>保存完整原图，图片不带检测框和文字。</p>
          <div className="dataset-option-grid">
            <label>抓图间隔（秒）<NumberField value={config.interval_sec} min={.3} max={86400} step="any" onChange={value => onChange({interval_sec: value ?? 2})} /></label>
            <label>每个文件夹图片张数<NumberField value={config.images_per_folder} min={1} max={10000} integerOnly onChange={value => onChange({images_per_folder: value ?? 30})} /></label>
            <label>最多保存几张<NumberField value={config.max_samples} min={0} max={1e9} integerOnly onChange={value => onChange({max_samples: value ?? 0})} /><small>0 表示不限张数</small></label>
            <label>运行时长（小时）<NumberField value={config.duration_hours} min={0} max={8760} step="any" onChange={value => onChange({duration_hours: value ?? 0})} /><small>0 表示持续运行，暂停不延后结束时间</small></label>
          </div>
          <details className="dataset-details"><summary>高级设置</summary><div className="dataset-advanced-options">
            <label>采集区域<select value={config.roi_name} onChange={event => onChange({roi_name: event.target.value})}>
              <option value="">整个推理范围</option>
              {source?.rois.map(roi => <option key={roi} value={roi}>{roi}</option>)}
            </select></label>
            <label>区域定位点<select value={config.roi_anchor} onChange={event => onChange({roi_anchor: event.target.value})}><option value="center">目标中心</option><option value="foot">脚点</option></select></label>
            <label>触发方式<select value={config.trigger_mode} onChange={event => onChange({trigger_mode: event.target.value})}><option value="periodic">满足条件时按间隔抓图</option><option value="on_enter">每次刚满足时抓一次</option></select></label>
            <label>条件持续时间（秒）<NumberField value={config.confirm_sec} min={0} max={60} step="any" onChange={value => onChange({confirm_sec: value ?? 0})} /></label>
          </div></details>
        </div>
      </fieldset>
      <div className="dataset-editor-footer">
        {error && <div className="dataset-banner error" role="alert">{error}</div>}
        <section className="dataset-capture-summary" aria-label="当前抓图逻辑">
          <strong>当前抓图逻辑 · 视频通道 {draft.channelId}</strong>
          <p>{config.rules.length > 1 ? '满足以下任意一种情况就抓图，同一帧只保存一张：' : '满足以下条件时抓图：'}</p>
          <ol>{(config.rules as Rule[]).map((rule, index) => <li key={rule.id || index}>{validCondition(rule.condition) ? summary(rule.condition, true) : '条件尚未配置完整'}</li>)}</ol>
          <p>判断范围：{config.roi_name ? `区域“${config.roi_name}”（按目标${config.roi_anchor === 'foot' ? '脚点' : '中心'}判断）` : '整个推理范围'}；{config.confirm_sec > 0 ? `条件持续满足 ${config.confirm_sec} 秒后` : '当帧满足即可'}，{config.trigger_mode === 'on_enter' ? '每种情况从不满足变为满足时抓一次，再次触发前需先不满足' : '满足期间按间隔持续抓图'}。两次抓图至少间隔 {config.interval_sec} 秒。</p>
          <p>保存完整原图，每文件夹 {config.images_per_folder} 张。{config.max_samples > 0 || config.duration_hours > 0 ? [config.max_samples > 0 ? `累计保存达到 ${config.max_samples} 张` : '', config.duration_hours > 0 ? `从首次开始起经过 ${config.duration_hours} 小时（暂停不延后结束时间）` : ''].filter(Boolean).join('，或') + '即停止。' : '不限张数和时长，手动暂停。'}</p>
        </section>
        {task?.enabled ? <><p>暂停后可修改配置。</p><button type="button" disabled={busy} onClick={() => onPause(task)}>暂停并编辑</button></>
          : !unavailable && <button type="submit" disabled={busy || !!saveReason}>{busy ? '正在保存…' : creating ? '创建任务' : '保存修改'}</button>}
      </div>
    </form>
  </section>
}
