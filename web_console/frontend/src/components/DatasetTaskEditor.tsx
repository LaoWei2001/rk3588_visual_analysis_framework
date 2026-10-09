import {useEffect, type RefObject} from 'react'
import type {DatasetConfig, DatasetStatus, DatasetTask} from '../api/dataset'
import DatasetRuleBuilder from './DatasetRuleBuilder'
import NumberField from './NumberField'

export interface DatasetDraft {
  taskId: string | null
  channelId: number
  config: DatasetConfig
  original: string
}

interface Props {
  draft: DatasetDraft
  status: DatasetStatus
  busy: boolean
  error: string
  panelRef: RefObject<HTMLElement>
  onChange: (values: Partial<DatasetConfig>) => void
  onChannelChange: (channel: number) => void
  onCancel: () => void
  onSave: () => void
  onPause: (task: DatasetTask) => void
}

export default function DatasetTaskEditor({draft, status, busy, error, panelRef: panel, onChange, onChannelChange, onCancel, onSave, onPause}: Props) {
  useEffect(() => {
    panel.current?.scrollIntoView({behavior: 'smooth', block: 'start'})
    panel.current?.querySelector('h2')?.focus({preventScroll: true})
  }, [panel])
  const creating = draft.taskId === null
  const task = status.tasks.find(task => task.id === draft.taskId)
  const source = status.channels.find(channel => channel.id === draft.channelId)
  const unavailable = !creating && !task
  const readOnly = busy || !!task?.enabled || unavailable
  const conflict = creating && status.tasks.some(task => task.channel_id === draft.channelId)
  const saveReason = unavailable ? '该任务已删除，请关闭配置。'
    : conflict ? '该通道已有采集任务，请取消新建并编辑已有任务。'
    : task && !task.enabled && task.pending > 0 ? '待传图片接收完毕后可保存修改。'
    : !creating || source?.eligible ? status.engine_error : '该通道当前不可用于采集。'
  const config = draft.config
  return <section className="dataset-config-panel" id="dataset-task-editor" ref={panel}>
    <form onSubmit={event => {event.preventDefault(); if (!readOnly && !saveReason) onSave()}}>
      <div className="dataset-section-heading">
        <h2 tabIndex={-1}>{creating ? '新建采集任务' : `${task?.enabled ? '查看' : '编辑'}采集任务 · 通道 ${draft.channelId}`}</h2>
        <button type="button" className="secondary" disabled={busy} onClick={onCancel}>{task?.enabled || unavailable ? '关闭' : '取消'}</button>
      </div>
      {error && <div className="dataset-banner error" role="alert">{error}</div>}
      {saveReason && <p className="dataset-editor-message">{saveReason}</p>}
      <div className="dataset-channel-picker">
        <label>采集通道<select aria-label="采集通道" value={draft.channelId} disabled={busy || !creating} onChange={event => onChannelChange(Number(event.target.value))}>
          {!source && <option value={draft.channelId}>通道 {draft.channelId} · 配置中未找到</option>}
          {status.channels.map(channel => {
            const occupied = status.tasks.some(task => task.channel_id === channel.id)
            return <option key={channel.id} value={channel.id} disabled={!channel.eligible || (creating && occupied)}>
              通道 {channel.id}{creating && occupied ? ' · 已有采集任务' : !channel.eligible ? ' · 需要单个检测模型' : ''}
            </option>
          })}
        </select></label>
        <p className="dataset-channel-labels">可用类别：{source?.labels.join('、') || '未读取到类别表'}</p>
      </div>
      <fieldset className="dataset-editor-grid" disabled={readOnly || conflict}>
        <div className="dataset-editor-conditions"><h3>抓图条件</h3>
          <DatasetRuleBuilder value={config.rules} onChange={rules => onChange({rules: rules as unknown[]})} />
        </div>
        <div className="dataset-editor-saving"><h3>保存与停止条件</h3>
          <div className="dataset-option-grid">
            <label>抓图间隔（秒）<NumberField value={config.interval_sec} min={.5} max={86400} step={.5} onChange={value => onChange({interval_sec: value ?? 2})} /></label>
            <label>每个文件夹图片张数<NumberField value={config.images_per_folder} min={1} max={10000} integerOnly onChange={value => onChange({images_per_folder: value ?? 30})} /></label>
            <label>最多保存几张<NumberField value={config.max_samples} min={0} max={1e9} integerOnly onChange={value => onChange({max_samples: value ?? 0})} /><small>0 表示不限张数</small></label>
            <label>运行时长（小时）<NumberField value={config.duration_hours} min={0} max={8760} step={1} onChange={value => onChange({duration_hours: value ?? 0})} /><small>0 表示持续运行，暂停不延后结束时间</small></label>
          </div>
          <details className="dataset-details"><summary>高级设置</summary><div className="dataset-advanced-options">
            <label>采集区域<select value={config.roi_name} onChange={event => onChange({roi_name: event.target.value})}>
              <option value="">整个推理范围</option>
              {source?.rois.map(roi => <option key={roi} value={roi}>{roi}</option>)}
            </select></label>
            <label>区域定位点<select value={config.roi_anchor} onChange={event => onChange({roi_anchor: event.target.value})}><option value="center">目标中心</option><option value="foot">脚点</option></select></label>
            <label>触发方式<select value={config.trigger_mode} onChange={event => onChange({trigger_mode: event.target.value})}><option value="periodic">满足条件时按间隔抓图</option><option value="on_enter">每次刚满足时抓一次</option></select></label>
            <label>条件持续时间（秒）<NumberField value={config.confirm_sec} min={0} max={60} step={.1} onChange={value => onChange({confirm_sec: value ?? 0})} /></label>
            <label>JPEG 质量<NumberField value={config.jpeg_quality} min={60} max={100} integerOnly onChange={value => onChange({jpeg_quality: value ?? 95})} /></label>
          </div></details>
        </div>
      </fieldset>
      <div className="dataset-editor-footer">
        {task?.enabled ? <><p>暂停后可修改配置。</p><button type="button" disabled={busy} onClick={() => onPause(task)}>暂停并编辑</button></>
          : !unavailable && <button type="submit" disabled={busy || !!saveReason}>{busy ? '正在保存…' : creating ? '创建任务' : '保存修改'}</button>}
      </div>
    </form>
  </section>
}
