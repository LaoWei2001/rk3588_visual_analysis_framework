export type Leaf = { class: string; min_confidence: number; count: { op: string; value: number } }
export type Condition = Leaf | { all: Condition[] } | { any: Condition[] } | { not: Condition }
export type Rule = { id: string; name: string; condition: Condition }
export const leaf = (name = '', absent = false): Leaf => ({ class: name, min_confidence: 0.3, count: { op: absent ? '==' : '>=', value: absent ? 0 : 1 } })
export const kindOf = (c: Condition) => 'all' in c ? 'all' : 'any' in c ? 'any' : 'not' in c ? 'not' : 'class'
export function summary(c: Condition): string {
  if ('all' in c) return `（${c.all.map(summary).join('，并且 ')}）`
  if ('any' in c) return `（${c.any.map(summary).join('，或者 ')}）`
  if ('not' in c) return `不满足 ${summary(c.not)}`
  if (c.count.op === '==' && c.count.value === 0) return `没检测到 ${c.class || '未填写的类别'}`
  if (c.count.op === '>=' && c.count.value === 1) return `检测到 ${c.class || '未填写的类别'}`
  const words: Record<string, string> = { '>=': '至少', '==': '刚好', '<=': '最多', '>': '超过', '<': '少于', '!=': '不是' }
  return `${c.class || '未填写的类别'} 的数量${words[c.count.op]} ${c.count.value} 个`
}
export function validCondition(c: unknown, depth = 0): c is Condition {
  if (!c || typeof c !== 'object' || depth > 8) return false
  const value = c as Record<string, unknown>
  const groups = ['all', 'any', 'not'].filter(key => key in value)
  if (groups.length > 1 || (groups.length && 'class' in value)) return false
  if ('all' in value || 'any' in value) {
    const children = value.all ?? value.any
    return Array.isArray(children) && children.length > 0 && children.every(child => validCondition(child, depth + 1))
  }
  if ('not' in value) return validCondition(value.not, depth + 1)
  const count = value.count as Record<string, unknown> | undefined
  return typeof value.class === 'string' && typeof value.min_confidence === 'number' && Number.isFinite(value.min_confidence) &&
    value.min_confidence >= 0 && value.min_confidence <= 1 && !!count &&
    ['==', '!=', '>=', '>', '<=', '<'].includes(String(count.op)) && typeof count.value === 'number' &&
    Number.isInteger(count.value) && count.value >= 0 && count.value <= 10000
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
