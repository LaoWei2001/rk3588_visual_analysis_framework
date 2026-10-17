export interface RoiLabel {
  id: string
  x: number
  y: number
  text: string
  color: string
  priority: number
}

export interface LabelRect { x: number; y: number; width: number; height: number }
export interface PlacedRoiLabel extends RoiLabel { rect: LabelRect }

function intersects(a: LabelRect, b: LabelRect, gap: number) {
  return a.x < b.x + b.width + gap && a.x + a.width + gap > b.x
    && a.y < b.y + b.height + gap && a.y + a.height + gap > b.y
}

/** Place labels across all zones together; leave dense overflow to vertex hover. */
export function layoutRoiLabels(labels: RoiLabel[], width: number, height: number, scale: number,
  measure: (text: string) => number, vertices: { x: number; y: number }[]): PlacedRoiLabel[] {
  const padding = 3 * scale, gap = 3 * scale, labelHeight = 19 * scale
  const markers = vertices.map(point => ({ x: point.x - 7 * scale, y: point.y - 7 * scale,
    width: 14 * scale, height: 14 * scale }))
  const occupied: LabelRect[] = []
  const placed = new Map<string, PlacedRoiLabel>()
  const ordered = labels.map((label, order) => ({ label, order }))
    .sort((a, b) => b.label.priority - a.label.priority || a.order - b.order)
  for (const { label } of ordered) {
    const labelWidth = measure(label.text) + padding * 2
    if (labelWidth > width || labelHeight > height) continue
    let rect: LabelRect | undefined
    // Prefer nearby positions, then spread clustered labels into wider rings.
    for (const distance of [10, 28, 50, 78, 112, 154]) {
      const radius = distance * scale
      const candidates = [
        [label.x + radius, label.y + radius],
        [label.x + radius, label.y - labelHeight - radius],
        [label.x - labelWidth - radius, label.y + radius],
        [label.x - labelWidth - radius, label.y - labelHeight - radius],
        [label.x - labelWidth / 2, label.y + radius],
        [label.x - labelWidth / 2, label.y - labelHeight - radius],
        [label.x + radius, label.y - labelHeight / 2],
        [label.x - labelWidth - radius, label.y - labelHeight / 2],
      ]
      for (const [x, y] of candidates) {
        const candidate = { x: Math.max(0, Math.min(width - labelWidth, x)),
          y: Math.max(0, Math.min(height - labelHeight, y)), width: labelWidth, height: labelHeight }
        if (occupied.some(other => intersects(candidate, other, gap))) continue
        if (markers.some(marker => intersects(candidate, marker, gap))) continue
        rect = candidate
        break
      }
      if (rect) break
    }
    if (!rect) {
      const slots: LabelRect[] = []
      for (let y = 0; y + labelHeight <= height; y += labelHeight + gap)
        for (let x = 0; x + labelWidth <= width; x += labelWidth + gap)
          slots.push({ x, y, width: labelWidth, height: labelHeight })
      slots.sort((a, b) => Math.hypot(a.x + labelWidth / 2 - label.x, a.y + labelHeight / 2 - label.y)
        - Math.hypot(b.x + labelWidth / 2 - label.x, b.y + labelHeight / 2 - label.y))
      rect = slots.find(candidate => !occupied.some(other => intersects(candidate, other, gap))
        && !markers.some(marker => intersects(candidate, marker, gap)))
    }
    // 当前查看/新增加的点必须可读；空间极紧时允许它靠近标记，但仍不覆盖其他标签。
    if (!rect && label.priority >= 2) {
      for (let y = 0; y + labelHeight <= height; y += labelHeight + gap) {
        for (let x = 0; x + labelWidth <= width; x += labelWidth + gap) {
          const candidate = { x, y, width: labelWidth, height: labelHeight }
          if (!occupied.some(other => intersects(candidate, other, gap))) { rect = candidate; break }
        }
        if (rect) break
      }
    }
    if (!rect) continue
    occupied.push(rect)
    placed.set(label.id, { ...label, rect })
  }
  return labels.flatMap(label => placed.has(label.id) ? [placed.get(label.id)!] : [])
}
