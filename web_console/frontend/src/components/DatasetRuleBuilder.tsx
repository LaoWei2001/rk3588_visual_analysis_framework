import NumberField from './NumberField'
import './DatasetRuleBuilder.css'

import { association, basicCondition, combine, comparison, kindOf, leaf, relationNames, summary, targetsFrom, validCondition, type Association, type Comparison, type Condition, type Leaf, type Rule, type Target } from './datasetRules'

function TargetConfidence({ label, value, onChange }: { label: string; value: Target; onChange: (v: Target) => void }) {
  return <label>{label}最低置信度（%）<NumberField value={Number((value.min_confidence * 100).toFixed(10))}
    min={0} max={100} step={1} onChange={v => onChange({ ...value, min_confidence: (v ?? 30) / 100 })} /></label>
}

function ComparisonEditor({ value, onChange }: { value: Comparison; onChange: (v: Comparison) => void }) {
  const v = value.compare_counts
  const set = (patch: Partial<Comparison['compare_counts']>) => onChange({ compare_counts: { ...v, ...patch } })
  return <>
    <label>类别 A<input aria-label="数量比较类别 A" value={v.left.class} placeholder="模型中的类别名" onChange={e => set({ left: { ...v.left, class: e.target.value } })} /></label>
    <label>A 的数量满足<select aria-label="两类数量关系" value={v.op} onChange={e => set({ op: e.target.value })}>
      {[['>', '大于 B 的数量'], ['>=', '不少于 B 的数量'], ['<', '小于 B 的数量'], ['<=', '不多于 B 的数量'], ['==', '等于 B 的数量'], ['!=', '不等于 B 的数量']].map(([op, label]) => <option key={op} value={op}>{label}</option>)}
    </select></label>
    <label>类别 B<input aria-label="数量比较类别 B" value={v.right.class} placeholder="模型中的另一类别名" onChange={e => set({ right: { ...v.right, class: e.target.value } })} /></label>
    <details className="dataset-nesting-tools"><summary>数量比较的高级设置</summary>
      <TargetConfidence label="类别 A" value={v.left} onChange={left => set({ left })} />
      <TargetConfidence label="类别 B" value={v.right} onChange={right => set({ right })} />
    </details>
  </>
}

function AssociationEditor({ value, onChange }: { value: Association; onChange: (v: Association) => void }) {
  const v = value.association, count = v.count ?? { op: '>=', value: 1 }
  const set = (patch: Partial<Association['association']>) => onChange({ association: { ...v, ...patch } })
  return <>
    <label>逐个检查的主体类别<input aria-label="主体类别" value={v.subject.class} placeholder="模型中的类别名" onChange={e => set({ subject: { ...v.subject, class: e.target.value } })} /></label>
    <div className="dataset-count-fields">
      <label>有多少主体满足<select aria-label="关联主体数量要求" value={v.quantifier ?? 'any'} onChange={e => set({ quantifier: e.target.value as 'any' | 'all' | 'count', ...(e.target.value === 'count' ? { count } : {}) })}>
        <option value="any">至少一个</option><option value="all">全部主体</option><option value="count">按数量判断…</option>
      </select></label>
      <label>什么情况下抓图<select aria-label="关联匹配状态" value={v.state} onChange={e => set({ state: e.target.value as 'matched' | 'unmatched' })}>
        <option value="unmatched">没有匹配到</option><option value="matched">匹配到了</option>
      </select></label>
    </div>
    <label>要匹配的关联类别<input aria-label="关联类别" value={v.object.class} placeholder="模型中的另一类别名" onChange={e => set({ object: { ...v.object, class: e.target.value } })} /></label>
    <label>怎样算匹配<select aria-label="空间匹配方式" value={v.relation} onChange={e => set({ relation: e.target.value as Association['association']['relation'] })}>
      {Object.entries(relationNames).map(([key, label]) => <option key={key} value={key}>{label}</option>)}
    </select></label>
    {v.quantifier === 'count' && <div className="dataset-count-fields">
      <label>满足条件的主体数量<select aria-label="关联数量比较" value={count.op} onChange={e => set({ count: { ...count, op: e.target.value } })}>
        {[['>=', '至少'], ['==', '刚好'], ['<=', '最多'], ['>', '超过'], ['<', '少于'], ['!=', '不是']].map(([op, label]) => <option key={op} value={op}>{label}</option>)}
      </select></label>
      <label>主体个数<NumberField value={count.value} integerOnly min={0} max={10000} onChange={n => set({ count: { ...count, value: n ?? 1 } })} /></label>
    </div>}
    <p className="dataset-field-help">没有检测到主体时不抓图。每个主体单独检查，其他主体匹配成功不会掩盖缺失。</p>
    <details className="dataset-nesting-tools"><summary>目标匹配的高级设置</summary>
      <TargetConfidence label="主体" value={v.subject} onChange={subject => set({ subject })} />
      <TargetConfidence label="关联目标" value={v.object} onChange={object => set({ object })} />
      <label className="dataset-inline-check"><input type="checkbox" aria-label="每个关联目标只分配给一个主体" checked={v.one_to_one !== false} onChange={e => set({ one_to_one: e.target.checked })} />每个关联目标只分配给一个主体</label>
      {v.relation === 'overlap' && <>
        <label>关联目标至少有多少面积在主体框内（%）<NumberField value={(v.min_overlap ?? 0.5) * 100} min={0.1} max={100} step={1} onChange={n => set({ min_overlap: Math.max(0.001, (n ?? 50) / 100) })} /></label>
        <p className="dataset-field-help">按关联目标框的面积计算，不按大主体框的面积计算。</p>
      </>}
      {v.relation === 'near' && <>
        <label>最大距离（主体框对角线的倍数）<NumberField value={v.max_distance ?? 1} min={0.01} max={100} step={0.1} onChange={n => set({ max_distance: Math.max(0.01, n ?? 1) })} /></label>
        <p className="dataset-field-help">比较两个框的中心距离。例如 1 表示距离不超过主体框的对角线长度。</p>
      </>}
    </details>
  </>
}

function ConditionEditor({ value, onChange, depth = 0 }: { value: Condition; onChange: (value: Condition) => void; depth?: number }) {
  const kind = kindOf(value)
  const canWrap = validCondition({ not: value }, depth)
  const [first, second] = targetsFrom(value)
  const typeSelector = <label>条件类型<select aria-label="条件类型" value={kind} onChange={e => onChange(e.target.value === 'association' ? association(first, second) : e.target.value === 'compare_counts' ? comparison(first, second) : { ...leaf(), ...first })}>
    <option value="class">类别出现、缺失或指定数量</option><option value="compare_counts">两类数量比较</option><option value="association">逐个目标检查关联</option>
  </select></label>
  if ('association' in value || 'compare_counts' in value) return <div className="dataset-condition dataset-detection">
    {typeSelector}
    {'association' in value ? <AssociationEditor value={value} onChange={onChange} /> : <ComparisonEditor value={value} onChange={onChange} />}
    <div className="dataset-condition-reading">当前条件：{summary(value)}</div>
    {canWrap && <details className="dataset-nesting-tools"><summary>需要组合或反向判断？</summary>
      <button type="button" onClick={() => onChange({ all: [value, leaf()] })}>与另一条条件组合</button>
      <button type="button" onClick={() => onChange({ not: value })}>这条条件不满足时成立</button>
    </details>}
  </div>
  if ('class' in value) {
    const mode = value.count.op === '==' && value.count.value === 0 ? 'absent'
      : value.count.op === '>=' && value.count.value === 1 ? 'present' : 'count'
    return <div className="dataset-condition dataset-detection">
      {typeSelector}
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
      <div className="dataset-condition-heading"><span>{['class', 'association', 'compare_counts'].includes(kindOf(child)) ? '条件' : '条件组'} {index + 1}</span>
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
  const direct = 'association' in rule.condition || 'compare_counts' in rule.condition
  const [first, second] = targetsFrom(rule.condition)
  return <>
    <label className="dataset-rule-mode">判断方式<select aria-label="抓图判断方式" value={basic ? 'presence' : direct ? kindOf(rule.condition) : 'advanced'} onChange={e => change(e.target.value === 'association' ? association(first, second) : e.target.value === 'compare_counts' ? comparison(first, second) : { all: [{ ...leaf(), ...first }] })}>
      <option value="presence">类别出现或缺失</option><option value="compare_counts">两类数量比较</option><option value="association">逐个目标检查关联</option>
      {!basic && !direct && <option value="advanced">数量或组合条件</option>}
    </select></label>
    {basic ? <>
      <div className="dataset-match" role="group" aria-label="条件之间的关系">
        <button type="button" aria-pressed={basic.mode === 'all'} onClick={() => change(combine('all', basic.leaves))}>全部满足</button>
        <button type="button" aria-pressed={basic.mode === 'any'} onClick={() => change(combine('any', basic.leaves))}>任一满足</button>
      </div>
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
    </> : direct ? <div className="dataset-condition dataset-detection dataset-direct-condition">
      {'association' in rule.condition ? <AssociationEditor value={rule.condition} onChange={change} /> : 'compare_counts' in rule.condition ? <ComparisonEditor value={rule.condition} onChange={change} /> : null}
      <details className="dataset-nesting-tools"><summary>需要再加条件或反向判断？</summary>
        <button type="button" onClick={() => change({ all: [rule.condition, leaf()] })}>与另一条条件组合</button>
        <button type="button" onClick={() => change({ not: rule.condition })}>这条条件不满足时成立</button>
      </details>
    </div> : <div className="dataset-complex-note">这组条件包含数量判断或嵌套组合，可展开下面的“更多条件设置”编辑。</div>}
    <div className="dataset-rule-preview"><span>抓图条件</span>{summary(rule.condition)} → 保存图片</div>
    <details className="dataset-details">
      <summary>更多条件设置</summary>
      <div className="dataset-details-body">
        <label className="dataset-rule-name">条件组备注<input aria-label="条件组名称" value={rule.name} onChange={e => onChange({ ...rule, name: e.target.value })} /></label>
        {!direct && <ConditionEditor value={rule.condition} onChange={change} />}
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
  </div>
}
