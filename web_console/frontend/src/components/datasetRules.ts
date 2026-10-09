export type Target = { class: string; min_confidence: number }
export type Count = { op: string; value: number }
export type Leaf = Target & { count: Count }
export type Comparison = { compare_counts: { left: Target; right: Target; op: string } }
export type Association = { association: {
  subject: Target; object: Target; relation: 'center_inside' | 'overlap' | 'near'; state: 'matched' | 'unmatched'
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
export function targetsFrom(c: Condition): Target[] {
  if ('class' in c) return [{ class: c.class, min_confidence: c.min_confidence }]
  if ('compare_counts' in c) return [c.compare_counts.left, c.compare_counts.right]
  if ('association' in c) return [c.association.subject, c.association.object]
  if ('not' in c) return targetsFrom(c.not)
  return ('all' in c ? c.all : c.any).flatMap(targetsFrom)
}
export const relationNames = { center_inside: '关联目标中心位于主体框内', overlap: '关联目标与主体重叠', near: '关联目标位于主体附近' }
const operators = ['==', '!=', '>=', '>', '<=', '<']
const countWords: Record<string, string> = { '>=': '至少', '==': '刚好', '<=': '最多', '>': '超过', '<': '少于', '!=': '不是' }
export function summary(c: Condition): string {
  if ('all' in c) return `（${c.all.map(summary).join('，并且 ')}）`
  if ('any' in c) return `（${c.any.map(summary).join('，或者 ')}）`
  if ('not' in c) return `不满足 ${summary(c.not)}`
  if ('compare_counts' in c) {
    const v = c.compare_counts
    const words: Record<string, string> = { '>': '大于', '>=': '不少于', '<': '小于', '<=': '不多于', '==': '等于', '!=': '不等于' }
    return `${v.left.class || '类别 A'} 的数量${words[v.op]} ${v.right.class || '类别 B'} 的数量`
  }
  if ('association' in c) {
    const v = c.association, count = v.count ?? { op: '>=', value: 1 }
    const quantity = v.quantifier === 'all' ? '全部' : v.quantifier === 'count' ? `${countWords[count.op]} ${count.value} 个` : '至少一个'
    return `${quantity} ${v.subject.class || '主体类别'} ${v.state === 'unmatched' ? '没有匹配到' : '匹配到'} ${v.object.class || '关联类别'}（${relationNames[v.relation]}${v.one_to_one === false ? '，允许复用关联目标' : '，一对一匹配'}）`
  }
  if (c.count.op === '==' && c.count.value === 0) return `没检测到 ${c.class || '未填写的类别'}`
  if (c.count.op === '>=' && c.count.value === 1) return `检测到 ${c.class || '未填写的类别'}`
  return `${c.class || '未填写的类别'} 的数量${countWords[c.count.op]} ${c.count.value} 个`
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
    return record(v) && validTarget(v.subject) && validTarget(v.object) &&
      ['center_inside', 'overlap', 'near'].includes(String(v.relation)) && ['matched', 'unmatched'].includes(String(v.state)) &&
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
