import type {DatasetConfig} from '../api/dataset'
import {summary, type Rule} from './datasetRules'

export default function DatasetTaskLogic({config}: {config: DatasetConfig}) {
  const rules = config.rules as Rule[]
  const limits = [
    config.max_samples > 0 ? `累计保存达到 ${config.max_samples} 张` : '',
    config.duration_hours > 0 ? `从首次开始采集起经过 ${config.duration_hours} 小时（暂停不延后结束时间）` : '',
  ].filter(Boolean)
  return <section className="dataset-task-logic" aria-label="取图逻辑">
    <h4>取图逻辑</h4>
    <p>{rules.length > 1 ? '满足以下任意一种情况即可抓图（或者），同一帧只保存一张：' : '满足以下情况时抓图：'}</p>
    <ol className="dataset-task-rules">
      {rules.map((rule, index) => <li key={rule.id}>
        <strong>情况 {index + 1}</strong>
        <p>{summary(rule.condition, true)}</p>
      </li>)}
    </ol>
    <dl className="dataset-task-policy">
      <div><dt>判断范围</dt><dd>{config.roi_name
        ? `采集区域“${config.roi_name}”，按${config.roi_anchor === 'foot' ? '目标脚点' : '目标中心'}判断是否在区域内`
        : '整个推理范围'}</dd></div>
      <div><dt>触发方式</dt><dd>{config.trigger_mode === 'on_enter'
        ? '每种情况从不满足变为满足时抓一次，再次触发前需先不满足'
        : '条件满足时按间隔持续抓图'}</dd></div>
      <div><dt>条件持续</dt><dd>{config.confirm_sec > 0
        ? `每种情况持续满足 ${config.confirm_sec} 秒后才抓图`
        : '当帧满足即可，无需持续等待'}</dd></div>
      <div><dt>抓图间隔</dt><dd>同一通道两次抓图至少间隔 {config.interval_sec} 秒</dd></div>
      <div><dt>保存方式</dt><dd>保存完整原图，不裁切区域、不叠加检测框或文字；每文件夹 {config.images_per_folder} 张</dd></div>
      <div><dt>停止条件</dt><dd>{limits.length
        ? `${limits.join('，或')}；达到${limits.length > 1 ? '任一' : ''}条件即停止`
        : '不限张数和时长，手动暂停'}；视觉程序停止时自动暂停，重新启动后需手动点击“开始采集”</dd></div>
    </dl>
  </section>
}
