import NumberField from './NumberField'
import './DatasetRuleBuilder.css'

import { association, associationTargets, basicCondition, combine, comparison, kindOf, leaf, relationNames, relationDescription, summary, targetsFrom, validCondition, type Association, type Comparison, type Condition, type Leaf, type Rule, type Target } from './datasetRules'

function TargetConfidence({ label, value, onChange }: { label: string; value: Target; onChange: (v: Target) => void }) {
  return <label>{label}最低置信度（%）<NumberField value={Number((value.min_confidence * 100).toFixed(10))}
    min={0} max={100} step="any" onChange={v => onChange({ ...value, min_confidence: (v ?? 30) / 100 })} /></label>
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
  const objects = associationTargets(v)
  const objectCount = v.object_count ?? { op: '>', value: 1 }
  const set = (patch: Partial<Association['association']>) => onChange({ association: { ...v, ...patch } })
  const setState = (state: Association['association']['state']) => {
    const { one_to_one, object_count: _objectCount, ...rest } = v
    onChange({ association: { ...rest, state, ...(state === 'count' ? { object_count: objectCount } : { one_to_one: one_to_one ?? true }) } })
  }
  const setObjects = (next: Target[]) => {
    const { object: _object, objects: _objects, ...rest } = v
    onChange({ association: { ...rest, ...(next.length === 1 ? { object: next[0] } : { objects: next }) } })
  }
  const updateObject = (index: number, next: Target) => setObjects(objects.map((t, i) => i === index ? next : t))
  return <>
    <label>逐个检查的主体类别<input aria-label="主体类别" value={v.subject.class} placeholder="模型中的类别名" onChange={e => set({ subject: { ...v.subject, class: e.target.value } })} /></label>
    <div className="dataset-count-fields">
      <label>有多少主体满足<select aria-label="关联主体数量要求" value={v.quantifier ?? 'any'} onChange={e => set({ quantifier: e.target.value as 'any' | 'all' | 'count', ...(e.target.value === 'count' ? { count } : {}) })}>
        <option value="any">至少一个</option><option value="all">全部主体</option><option value="count">按数量判断…</option>
      </select></label>
      <label>什么情况下抓图<select aria-label="关联匹配状态" value={v.state} onChange={e => setState(e.target.value as Association['association']['state'])}>
        <option value="unmatched">{objects.length > 1 ? '以下类别都没匹配到' : '没有匹配到'}</option><option value="matched">{objects.length > 1 ? '匹配到以下任意一类' : '匹配到了'}</option>
        <option value="count">按每个主体的关联目标数量判断…</option>
      </select></label>
    </div>
    {objects.map((object, index) => <div className="dataset-association-target" key={index}>
      <label>{objects.length === 1 ? '要匹配的关联类别' : `关联类别 ${index + 1}`}<input aria-label={index === 0 ? '关联类别' : `关联类别 ${index + 1}`} value={object.class} placeholder="模型中的另一类别名" onChange={e => updateObject(index, { ...object, class: e.target.value })} /></label>
      {objects.length > 1 && <button type="button" className="dataset-remove" aria-label={`删除关联类别 ${index + 1}`} onClick={() => setObjects(objects.filter((_, i) => i !== index))}>×</button>}
    </div>)}
    <button type="button" className="dataset-add" disabled={objects.length >= 32} onClick={() => setObjects([...objects, { class: '', min_confidence: v.subject.min_confidence }])}>＋ 添加关联类别</button>
    {new Set(objects.map(t => t.class)).size !== objects.length && <div className="dataset-rule-error">关联类别不能重复，请修改或删除重复项。</div>}
    {v.state === 'count' ? <>
      <div className="dataset-count-fields">
        <label>每个主体的关联目标数量<select aria-label="关联目标数量比较" value={objectCount.op} onChange={e => set({ object_count: { ...objectCount, op: e.target.value } })}>
          {[['>', '超过'], ['>=', '至少'], ['==', '刚好'], ['<=', '最多'], ['<', '少于'], ['!=', '不是']].map(([op, label]) => <option key={op} value={op}>{label}</option>)}
        </select></label>
        <label>关联目标个数<NumberField value={objectCount.value} integerOnly min={0} max={10000} onChange={n => set({ object_count: { ...objectCount, value: n ?? 1 } })} /></label>
      </div>
      <p className="dataset-field-help">逐个主体统计关联目标，各类别合计；重叠主体可能计入同一个目标。</p>
    </> : <p className="dataset-field-help">任意一类匹配成功即算匹配；“都没匹配到”要求所有类别均无匹配。</p>}
    <label>怎样算匹配<select aria-label="空间匹配方式" value={v.relation} onChange={e => set({ relation: e.target.value as Association['association']['relation'] })}>
      {Object.keys(relationNames).map(key => <option key={key} value={key}>{relationDescription({...v, relation: key as Association['association']['relation']})}</option>)}
    </select></label>
    <p className="dataset-field-help dataset-relation-explanation">匹配方向：{relationDescription(v)}。</p>
    {(v.relation === 'overlap' || v.relation === 'subject_overlap') && <p className="dataset-field-help">
      当前按 {v.relation === 'subject_overlap' ? v.subject.class || '主体' : objects.map(t => t.class || '关联目标').join('、')} 的面积计算。
    </p>}
    {v.state !== 'count' && (v.relation === 'subject_center_inside' || v.relation === 'subject_overlap') && v.one_to_one !== false &&
      <p className="dataset-field-help">一对一：每个 {objects.map(t => t.class || '关联目标').join('、')} 最多匹配一个 {v.subject.class || '主体'}。</p>}
    {v.quantifier === 'count' && <div className="dataset-count-fields">
      <label>满足条件的主体数量<select aria-label="关联数量比较" value={count.op} onChange={e => set({ count: { ...count, op: e.target.value } })}>
        {[['>=', '至少'], ['==', '刚好'], ['<=', '最多'], ['>', '超过'], ['<', '少于'], ['!=', '不是']].map(([op, label]) => <option key={op} value={op}>{label}</option>)}
      </select></label>
      <label>主体个数<NumberField value={count.value} integerOnly min={0} max={10000} onChange={n => set({ count: { ...count, value: n ?? 1 } })} /></label>
    </div>}
    <p className="dataset-field-help">没有主体时不抓图；各主体独立判断。</p>
    <details className="dataset-nesting-tools"><summary>目标匹配的高级设置</summary>
      <TargetConfidence label="主体" value={v.subject} onChange={subject => set({ subject })} />
      {objects.map((object, index) => <TargetConfidence key={index} label={objects.length === 1 ? '关联目标' : `关联类别 ${index + 1}（${object.class || '未填写'}）`} value={object} onChange={next => updateObject(index, next)} />)}
      {v.state !== 'count' && <>
        <label className="dataset-inline-check"><input type="checkbox" aria-label="每个关联目标只分配给一个主体" checked={v.one_to_one !== false} onChange={e => set({ one_to_one: e.target.checked })} />每个 {objects.map(t => t.class || '关联目标').join('、')} 最多匹配一个 {v.subject.class || '主体'}</label>
        <p className="dataset-field-help">取消勾选可共用关联框；一对一分配不足时视为未匹配。</p>
      </>}
      {(v.relation === 'overlap' || v.relation === 'subject_overlap') && <>
        <label>{v.relation === 'subject_overlap' ? '主体至少有多少面积在关联目标框内（%）' : '关联目标至少有多少面积在主体框内（%）'}<NumberField value={(v.min_overlap ?? 0.5) * 100} min={0.1} max={100} step="any" onChange={n => set({ min_overlap: Math.max(0.001, (n ?? 50) / 100) })} /></label>
        <p className="dataset-field-help">计算方式：两框交集面积 ÷ {v.relation === 'subject_overlap' ? '主体' : '关联目标'}框面积。</p>
      </>}
      {v.relation === 'near' && <>
        <label>最大距离（主体框对角线的倍数）<NumberField value={v.max_distance ?? 1} min={0.01} max={100} step="any" onChange={n => set({ max_distance: Math.max(0.01, n ?? 1) })} /></label>
        <p className="dataset-field-help">按两框中心距离计算，1 表示主体框对角线长度。</p>
      </>}
    </details>
  </>
}

function ExtraRequirements({value, onAdd, onInvert}: {
  value: Condition; onAdd: () => void; onInvert: () => void
}) {
  return <details className="dataset-nesting-tools">
    <summary>添加条件，或在条件不满足时抓图</summary>
    <button type="button" onClick={onAdd}>再加一条必须满足的条件</button>
    <p className="dataset-field-help">当前条件和新增条件都满足时，才抓图。</p>
    <button type="button" onClick={onInvert}>改成不满足当前条件时抓图</button>
    <p className="dataset-field-help">改后抓图条件：{summary({not: value})}</p>
  </details>
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
    {canWrap && <ExtraRequirements value={value} onAdd={() => onChange({all: [value, leaf()]})}
      onInvert={() => onChange({not: value})} />}
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
      <label>最低检测置信度（%）<NumberField value={Number((value.min_confidence * 100).toFixed(10))} min={0} max={100} step="any"
        onChange={v => onChange({ ...value, min_confidence: (v ?? 30) / 100 })} /></label>
      <div className="dataset-condition-reading">当前条件：{summary(value)}</div>
      {canWrap && <ExtraRequirements value={value}
        onAdd={() => onChange({all: [value, {...leaf(), min_confidence: value.min_confidence}]})}
        onInvert={() => onChange({not: value})} />}
    </div>
  }
  if ('not' in value) return <div className="dataset-condition dataset-condition-group">
    <div className="dataset-group-title">不符合下面的条件时抓图</div>
    <p className="dataset-field-help">以下条件整体取反。</p>
    <div className="dataset-condition-reading">当前抓图条件：{summary(value)}</div>
    <button type="button" className="dataset-group-action" onClick={() => onChange(value.not)}>恢复为满足下面条件时抓图</button>
    <ConditionEditor value={value.not} depth={depth + 1} onChange={next => onChange({ not: next })} />
  </div>
  const children = 'all' in value ? value.all : value.any
  const setChildren = (next: Condition[]) => onChange(kind === 'all' ? { all: next } : { any: next })
  return <div className="dataset-condition dataset-condition-group">
    <label className="dataset-group-relation">这些条件的关系<select aria-label="条件组合方式" value={kind}
      onChange={e => onChange(e.target.value === 'all' ? { all: children } : { any: children })}>
      <option value="all">需要同时满足下面每一条</option>
      <option value="any">满足下面任意一条即可</option>
    </select></label>
    {children.map((child, index) => <div className="dataset-condition-child" key={index}>
      <div className="dataset-condition-heading"><span>{['class', 'association', 'compare_counts'].includes(kindOf(child)) ? '条件' : '条件组'} {index + 1}</span>
        {children.length > 1 && <button type="button" aria-label={`删除高级条件 ${index + 1}`} onClick={() => setChildren(children.filter((_, i) => i !== index))}>删除</button>}
      </div>
      <ConditionEditor value={child} depth={depth + 1} onChange={next => setChildren(children.map((old, i) => i === index ? next : old))} />
    </div>)}
    {children.length < 32 && <button type="button" className="dataset-add dataset-group-action" onClick={() => setChildren([...children, leaf()])}>＋ 添加检测条件</button>}
    {(depth < 7 || canWrap) && <details className="dataset-nesting-tools">
      <summary>添加可选条件，或在整组条件不满足时抓图</summary>
      {depth < 7 && children.length < 32 && <>
        <button type="button" onClick={() => setChildren([...children, { any: [leaf(), leaf()] }])}>添加一组“满足其中一条即可”的条件</button>
        <p className="dataset-field-help">新增组中满足一条即可，再按上面选择的关系与其他条件一起判断。</p>
      </>}
      {canWrap && <>
        <button type="button" onClick={() => onChange({ not: value })}>改成不满足这整组条件时抓图</button>
        <p className="dataset-field-help">改后抓图条件：{summary({not: value})}</p>
      </>}
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
        <button type="button" aria-pressed={basic.mode === 'all'} onClick={() => change(combine('all', basic.leaves))}>全部满足才抓图</button>
        <button type="button" aria-pressed={basic.mode === 'any'} onClick={() => change(combine('any', basic.leaves))}>满足任意一条就抓图</button>
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
      <ExtraRequirements value={rule.condition} onAdd={() => change({all: [rule.condition, leaf()]})}
        onInvert={() => change({not: rule.condition})} />
    </div> : <div className="dataset-complex-note">这组条件包含数量判断或嵌套组合，可展开下面的“更多条件设置”编辑。</div>}
    <div className="dataset-rule-preview"><span>抓图条件</span>{summary(rule.condition)} → 保存图片</div>
    {!direct && <details className="dataset-details">
      <summary>更多条件设置</summary>
      <div className="dataset-details-body">
        <ConditionEditor value={rule.condition} onChange={change} />
      </div>
    </details>}
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
    {rules.length < 32 && <>
      <button type="button" className="dataset-add" onClick={() => onChange([...rules, { id: `rule_${Date.now()}_${rules.length}`, name: `采集条件 ${rules.length + 1}`, condition: leaf() }])}>＋ 添加另一种抓图情况</button>
      <p className="dataset-field-help">满足任意一组即可抓图。</p>
    </>}
  </div>
}
