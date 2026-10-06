import NumberField from './NumberField'
import './DatasetRuleBuilder.css'

import { basicCondition, combine, kindOf, leaf, summary, validCondition, type Condition, type Leaf, type Rule } from './datasetRules'

function ConditionEditor({ value, onChange, depth = 0 }: { value: Condition; onChange: (value: Condition) => void; depth?: number }) {
  const kind = kindOf(value)
  const canWrap = validCondition({ not: value }, depth)
  if ('class' in value) {
    const mode = value.count.op === '==' && value.count.value === 0 ? 'absent'
      : value.count.op === '>=' && value.count.value === 1 ? 'present' : 'count'
    return <div className="dataset-condition dataset-detection">
      <label>要判断的类别<input aria-label="类别名称" value={value.class} placeholder="模型标签，如 person" onChange={e => onChange({ ...value, class: e.target.value })} /></label>
      <label>什么情况下满足<select aria-label="检测条件" value={mode} onChange={e => onChange({ ...value, count:
        e.target.value === 'absent' ? { op: '==', value: 0 } : e.target.value === 'present' ? { op: '>=', value: 1 } : { op: '>=', value: 2 } })}>
        <option value="present">检测到这个类别（至少 1 个）</option>
        <option value="absent">没检测到这个类别（0 个）</option>
        <option value="count">按指定数量判断…</option>
      </select></label>
      {mode === 'count' && <div className="dataset-count-fields">
        <label>数量要求<select aria-label="数量比较" value={value.count.op} onChange={e => onChange({ ...value, count: { ...value.count, op: e.target.value } })}>
          {[['>=', '至少'], ['==', '刚好'], ['<=', '最多'], ['>', '超过'], ['<', '少于'], ['!=', '不是']].map(([op, name]) => <option key={op} value={op}>{name}</option>)}
        </select></label>
        <label>目标个数<NumberField value={value.count.value} integerOnly min={0} max={10000} step={1} onChange={v => onChange({ ...value, count: { ...value.count, value: v ?? 0 } })} /></label>
      </div>}
      <label>最低检测置信度（%）<NumberField value={Number((value.min_confidence * 100).toFixed(10))} min={0} max={100} step={1}
        onChange={v => onChange({ ...value, min_confidence: (v ?? 30) / 100 })} /></label>
      <p className="dataset-field-help">得分达到这个值的目标才计入数量。30 表示 30%。</p>
      <div className="dataset-condition-reading">当前条件：{summary(value)}</div>
      {canWrap && <details className="dataset-nesting-tools">
        <summary>需要组合或反向判断？</summary>
        <button type="button" onClick={() => onChange({ all: [value, { ...leaf(), min_confidence: value.min_confidence }] })}>与另一条条件组合</button>
        <button type="button" onClick={() => onChange({ not: value })}>这条条件不满足时成立</button>
      </details>}
    </div>
  }
  if ('not' in value) return <div className="dataset-condition dataset-condition-group">
    <div className="dataset-group-title">下面的条件不满足时，这一组才成立</div>
    <p className="dataset-field-help">例如：下面是“检测到 helmet”，反向判断就是“没检测到 helmet”。</p>
    <button type="button" className="dataset-group-action" onClick={() => onChange(value.not)}>取消反向判断</button>
    <ConditionEditor value={value.not} depth={depth + 1} onChange={next => onChange({ not: next })} />
  </div>
  const children = 'all' in value ? value.all : value.any
  const setChildren = (next: Condition[]) => onChange(kind === 'all' ? { all: next } : { any: next })
  return <div className="dataset-condition dataset-condition-group">
    <label className="dataset-group-relation">这组条件如何满足<select aria-label="条件组合方式" value={kind}
      onChange={e => onChange(e.target.value === 'all' ? { all: children } : { any: children })}>
      <option value="all">下面每一条都满足，才成立</option>
      <option value="any">下面至少一条满足，就成立</option>
    </select></label>
    {children.map((child, index) => <div className="dataset-condition-child" key={index}>
      <div className="dataset-condition-heading"><span>{'class' in child ? '条件' : '条件组'} {index + 1}</span>
        {children.length > 1 && <button type="button" aria-label={`删除高级条件 ${index + 1}`} onClick={() => setChildren(children.filter((_, i) => i !== index))}>删除</button>}
      </div>
      <ConditionEditor value={child} depth={depth + 1} onChange={next => setChildren(children.map((old, i) => i === index ? next : old))} />
    </div>)}
    {children.length < 32 && <button type="button" className="dataset-add dataset-group-action" onClick={() => setChildren([...children, leaf()])}>＋ 添加检测条件</button>}
    {(depth < 7 || canWrap) && <details className="dataset-nesting-tools">
      <summary>需要嵌套组合或反向判断？</summary>
      {depth < 7 && children.length < 32 && <button type="button" onClick={() => setChildren([...children, { any: [leaf(), leaf()] }])}>＋ 添加条件组</button>}
      {canWrap && <button type="button" onClick={() => onChange({ not: value })}>这组条件不满足时成立</button>}
    </details>}
  </div>
}

function BasicRule({ rule, onChange }: { rule: Rule; onChange: (rule: Rule) => void }) {
  const basic = basicCondition(rule.condition)
  const change = (condition: Condition) => onChange({ ...rule, condition })
  return <>
    {basic ? <>
      <div className="dataset-match" role="group" aria-label="条件之间的关系">
        <button type="button" aria-pressed={basic.mode === 'all'} onClick={() => change(combine('all', basic.leaves))}>全部满足</button>
        <button type="button" aria-pressed={basic.mode === 'any'} onClick={() => change(combine('any', basic.leaves))}>任一满足</button>
      </div>
      <p className="dataset-match-hint">{basic.mode === 'all' ? '下面每一条都满足，才保存图片。' : '下面只要有一条满足，就保存图片。'}</p>
      {basic.leaves.map((item, index) => <div className="dataset-basic-row" key={index}>
        <select aria-label={`条件 ${index + 1} 检测状态`} value={item.count.value === 0 ? 'absent' : 'present'} onChange={e => {
          const next: Leaf = { ...item, count: e.target.value === 'absent' ? { op: '==', value: 0 } : { op: '>=', value: 1 } }
          change(combine(basic.mode, basic.leaves.map((old, i) => i === index ? next : old)))
        }}>
          <option value="present">检测到</option><option value="absent">没检测到</option>
        </select>
        <input aria-label={`条件 ${index + 1} 类别`} placeholder="类别名，如 person" value={item.class}
          onChange={e => change(combine(basic.mode, basic.leaves.map((old, i) => i === index ? { ...old, class: e.target.value } : old)))} />
        <button type="button" className="dataset-remove" aria-label={`删除条件 ${index + 1}`} disabled={basic.leaves.length === 1}
          onClick={() => change(combine(basic.mode, basic.leaves.filter((_, i) => i !== index)))}>×</button>
      </div>)}
      {basic.leaves.some(item => !item.class.trim()) && <div className="dataset-rule-error">请填写类别名，与模型标签一致。</div>}
      {basic.leaves.length < 32 && <button type="button" className="dataset-add" onClick={() => change(combine(basic.mode, [...basic.leaves, { ...leaf(), min_confidence: Math.min(...basic.leaves.map(item => item.min_confidence)) }]))}>＋ 添加类别</button>}
    </> : <div className="dataset-complex-note">这组条件包含数量判断或嵌套组合，可展开下面的“更多条件设置”编辑。</div>}
    <div className="dataset-rule-preview"><span>抓图条件</span>{summary(rule.condition)} → 保存图片</div>
    <details className="dataset-details">
      <summary>更多条件设置</summary>
      <div className="dataset-details-body">
        <p className="dataset-match-hint">逐条设置“要判断什么”和“什么情况下满足”。下方会显示实际抓图条件。</p>
        <label className="dataset-rule-name">条件组名称（仅用于备注）<input aria-label="条件组名称" value={rule.name} onChange={e => onChange({ ...rule, name: e.target.value })} /></label>
        <ConditionEditor value={rule.condition} onChange={change} />
      </div>
    </details>
  </>
}
export default function DatasetRuleBuilder({ value, onChange }: { value: unknown; onChange: (value: unknown) => void }) {
  const valid = Array.isArray(value) && value.length > 0 && value.every(rule => rule && typeof rule === 'object' &&
    typeof rule.id === 'string' && typeof rule.name === 'string' && validCondition(rule.condition))
  if (!valid) return <div className="ncp-hint">当前条件格式无法编辑。<button type="button" onClick={() => onChange([{ id: 'rule_1', name: '采集条件', condition: { all: [leaf('person'), leaf('helmet', true)] } }])}>创建采集条件</button></div>
  const rules = value as Rule[]
  return <div className="dataset-rule-builder">
    {rules.map((rule, index) => <section className="dataset-rule" aria-label={`采集条件组 ${index + 1}`} key={rule.id}>
      {rules.length > 1 && <div className="dataset-rule-heading"><span>{index === 0 ? '条件组 1' : `或者满足条件组 ${index + 1}`}</span>
        <button type="button" aria-label={`删除条件组 ${index + 1}`} onClick={() => onChange(rules.filter((_, i) => i !== index))}>删除组</button>
      </div>}
      <BasicRule rule={rule} onChange={next => onChange(rules.map((old, i) => i === index ? next : old))} />
    </section>)}
    {rules.length < 32 && <button type="button" className="dataset-add" onClick={() => onChange([...rules, { id: `rule_${Date.now()}_${rules.length}`, name: `采集条件 ${rules.length + 1}`, condition: leaf() }])}>＋ 再加一组条件（或）</button>}
    <div className="dataset-match-hint">类别名与模型标签一致。判断的是模型检测结果。</div>
  </div>
}
