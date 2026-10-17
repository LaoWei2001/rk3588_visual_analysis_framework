export type Target = { class: string; min_confidence: number }
export type Count = { op: string; value: number }
export type Leaf = Target & { count: Count }
export type Comparison = { compare_counts: { left: Target; right: Target; op: string } }
export type Association = { association: {
  subject: Target; object?: Target; objects?: Target[]; relation: 'center_inside' | 'subject_center_inside' | 'overlap' | 'subject_overlap' | 'near'; state: 'matched' | 'unmatched' | 'count'
  object_count?: Count
  quantifier?: 'any' | 'all' | 'count'; count?: Count; one_to_one?: boolean; min_overlap?: number; max_distance?: number
} }
export type Condition = Leaf | Comparison | Association | { all: Condition[] } | { any: Condition[] } | { not: Condition }
export type Rule = { id: string; name: string; condition: Condition }
export const leaf = (name = '', absent = false): Leaf => ({ class: name, min_confidence: 0.3, count: { op: absent ? '==' : '>=', value: absent ? 0 : 1 } })
export const kindOf = (c: Condition) => 'all' in c ? 'all' : 'any' in c ? 'any' : 'not' in c ? 'not' : 'compare_counts' in c ? 'compare_counts' : 'association' in c ? 'association' : 'class'
export const target = (): Target => ({ class: '', min_confidence: 0.3 })
export const comparison = (left: Target = target(), right: Target = target()): Comparison => ({ compare_counts: { left, right, op: '>' } })
export const association = (subject: Target = target(), object: Target = target()): Association => ({ association: {
  subject, object, relation: 'center_inside', state: 'unmatched', quantifier: 'any', one_to_one: true, min_overlap: 0.5, max_distance: 1,
} })
// 兼容已保存的单类别规则；多类别共用一个匹配池，任意一类匹配成功即可。
export const associationTargets = (v: Association['association']): Target[] => v.objects ?? (v.object ? [v.object] : [])
export function targetsFrom(c: Condition): Target[] {
  if ('class' in c) return [{ class: c.class, min_confidence: c.min_confidence }]
  if ('compare_counts' in c) return [c.compare_counts.left, c.compare_counts.right]
  if ('association' in c) return [c.association.subject, ...associationTargets(c.association)]
  if ('not' in c) return targetsFrom(c.not)
  return ('all' in c ? c.all : c.any).flatMap(targetsFrom)
}
export const relationNames = {
  center_inside: '关联目标的中心在主体框内',
  subject_center_inside: '主体的中心在关联目标框内',
  overlap: '按关联目标面积判断：关联目标在主体框内',
  subject_overlap: '按主体面积判断：主体在关联目标框内',
  near: '按主体框对角线判断中心距离',
}
const operators = ['==', '!=', '>=', '>', '<=', '<']
const countWords: Record<string, string> = { '>=': '至少', '==': '刚好', '<=': '最多', '>': '超过', '<': '少于', '!=': '不是' }
const percent = (value: number) => Number((value * 100).toFixed(10))
const targetName = (value: Target, fallback: string, detailed: boolean) =>
  `${value.class || fallback}${detailed ? `（置信度 ≥ ${percent(value.min_confidence ?? .3)}%）` : ''}`
export function relationDescription(v: Association['association']): string {
  const subject = v.subject.class || '主体', objects = associationTargets(v).map(t => t.class || '关联目标').join(' 或 ')
  if (v.relation === 'center_inside') return `${objects} 的中心在 ${subject} 框内`
  if (v.relation === 'subject_center_inside') return `${subject} 的中心在 ${objects} 框内`
  if (v.relation === 'overlap') return `${objects} 至少 ${percent(v.min_overlap ?? .5)}% 的面积在 ${subject} 框内`
  if (v.relation === 'subject_overlap') return `${subject} 至少 ${percent(v.min_overlap ?? .5)}% 的面积在 ${objects} 框内`
  return `${subject} 与 ${objects} 的中心距离不超过 ${subject} 框对角线的 ${v.max_distance ?? 1} 倍`
}
export function summary(c: Condition, detailed = false): string {
  if ('all' in c) return `（${c.all.map(child => summary(child, detailed)).join('，并且 ')}）`
  if ('any' in c) return `（${c.any.map(child => summary(child, detailed)).join('，或者 ')}）`
  if ('not' in c) {
    const child = c.not
    if ('not' in child) return summary(child.not, detailed)
    if ('class' in child) {
      if (child.count.op === '>=' && child.count.value === 1) return `没检测到 ${targetName(child, '未填写的类别', detailed)}`
      if (child.count.op === '==' && child.count.value === 0) return `检测到 ${targetName(child, '未填写的类别', detailed)}`
      const opposite: Record<string, string> = {'==': '!=', '!=': '==', '>': '<=', '>=': '<', '<': '>=', '<=': '>'}
      return summary({...child, count: {...child.count, op: opposite[child.count.op]}}, detailed)
    }
    return detailed ? `（以下情况不成立：${summary(child, true)}）` : `没有出现以下情况：${summary(child)}`
  }
  if ('compare_counts' in c) {
    const v = c.compare_counts
    const words: Record<string, string> = { '>': '大于', '>=': '不少于', '<': '小于', '<=': '不多于', '==': '等于', '!=': '不等于' }
    return `${targetName(v.left, '类别 A', detailed)} 的数量${words[v.op]} ${targetName(v.right, '类别 B', detailed)} 的数量`
  }
  if ('association' in c) {
    const v = c.association, count = v.count ?? { op: '>=', value: 1 }
    const quantity = v.quantifier === 'all' ? '全部' : v.quantifier === 'count' ? `${countWords[count.op]} ${count.value} 个` : '至少一个'
    const objects = associationTargets(v), names = objects.map(t => targetName(t, '关联类别', detailed)).join('、')
    const subject = targetName(v.subject, '主体类别', detailed)
    const relation = relationDescription(v)
    const subjectRequired = detailed ? '；没有主体时不满足此条件' : ''
    if (v.state === 'count') {
      const requirement = v.object_count ?? { op: '>', value: 1 }
      return `${quantity} ${subject} 的 ${names} 关联目标${objects.length > 1 ? '总' : ''}数量${countWords[requirement.op]} ${requirement.value} 个（${relation}，逐主体统计${subjectRequired}）`
    }
    const state = objects.length > 1 ? (v.state === 'unmatched' ? '没有匹配到以下任何一类：' : '匹配到以下至少一类：') : (v.state === 'unmatched' ? '没有匹配到 ' : '匹配到 ')
    const allocation = v.one_to_one === false ? '允许复用关联目标' : `一对一匹配${detailed ? `：每个 ${objects.map(t => t.class || '关联目标').join('、')} 最多匹配一个 ${v.subject.class || '主体'}` : ''}`
    return `${quantity} ${subject} ${state}${names}（${relation}，${allocation}${subjectRequired}）`
  }
  const name = targetName(c, '未填写的类别', detailed)
  if (c.count.op === '==' && c.count.value === 0) return `没检测到 ${name}`
  if (c.count.op === '>=' && c.count.value === 1) return `检测到 ${name}`
  return `${name} 的数量${countWords[c.count.op]} ${c.count.value} 个`
}
const record = (v: unknown): v is Record<string, unknown> => !!v && typeof v === 'object' && !Array.isArray(v)
const validTarget = (v: unknown): v is Target => record(v) && typeof v.class === 'string' && typeof v.min_confidence === 'number' && Number.isFinite(v.min_confidence) && v.min_confidence >= 0 && v.min_confidence <= 1
const validCount = (v: unknown): v is Count => record(v) && operators.includes(String(v.op)) && typeof v.value === 'number' && Number.isInteger(v.value) && v.value >= 0 && v.value <= 10000
export function validCondition(c: unknown, depth = 0): c is Condition {
  if (!c || typeof c !== 'object' || depth > 8) return false
  const value = c as Record<string, unknown>
  const kinds = ['all', 'any', 'not', 'class', 'compare_counts', 'association'].filter(key => key in value)
  if (kinds.length !== 1) return false
  if ('all' in value || 'any' in value) {
    const children = value.all ?? value.any
    return Array.isArray(children) && children.length > 0 && children.every(child => validCondition(child, depth + 1))
  }
  if ('not' in value) return validCondition(value.not, depth + 1)
  if ('compare_counts' in value) {
    const v = value.compare_counts
    return record(v) && validTarget(v.left) && validTarget(v.right) && operators.includes(String(v.op))
  }
  if ('association' in value) {
    const v = value.association
    if (!record(v) || !validTarget(v.subject) || ('object' in v) === ('objects' in v)) return false
    const objects = 'objects' in v ? v.objects : [v.object]
    return Array.isArray(objects) && objects.length >= 1 && objects.length <= 32 && objects.every(validTarget) &&
      ['center_inside', 'subject_center_inside', 'overlap', 'subject_overlap', 'near'].includes(String(v.relation)) && ['matched', 'unmatched', 'count'].includes(String(v.state)) &&
      (v.state === 'count' ? validCount(v.object_count) && v.one_to_one === undefined : v.object_count === undefined) &&
      (v.quantifier === undefined || ['any', 'all', 'count'].includes(String(v.quantifier))) &&
      (v.count === undefined ? v.quantifier !== 'count' : validCount(v.count)) &&
      (v.one_to_one === undefined || typeof v.one_to_one === 'boolean') &&
      (v.min_overlap === undefined || typeof v.min_overlap === 'number' && v.min_overlap > 0 && v.min_overlap <= 1) &&
      (v.max_distance === undefined || typeof v.max_distance === 'number' && v.max_distance > 0 && v.max_distance <= 100)
  }
  return validCount(value.count) && validTarget(value)
}
export function basicCondition(c: Condition): { mode: 'all' | 'any'; leaves: Leaf[] } | null {
  const mode = 'any' in c ? 'any' : 'all'
  const children = 'all' in c ? c.all : 'any' in c ? c.any : [c]
  // 只对等价的“有/没有”条件提供简洁编辑，复杂条件必须保留原结构与阈值。
  const basic = children.every(child => 'class' in child &&
    ((child.count.op === '>=' && child.count.value === 1) || (child.count.op === '==' && child.count.value === 0)))
  return basic ? { mode, leaves: children as Leaf[] } : null
}
export const combine = (mode: 'all' | 'any', leaves: Leaf[]): Condition => mode === 'all' ? { all: leaves } : { any: leaves }
