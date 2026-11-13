/** Pure inference ROI geometry and configuration conversion. No UI or transport dependencies. */
export interface NormalizedRect { x: number; y: number; width: number; height: number }
export type NormalizedPoint = [number, number]
export type InferenceRoiMode = 'roi_only' | 'full_plus_roi' | 'full_frame_roi_filter'
export type InferenceRoiShape = 'rect' | 'polygon'
export type InferenceResizeMode = 'stretch' | 'expand' | 'letterbox'
export interface InferenceRoiValue {
  mode: InferenceRoiMode
  shape: InferenceRoiShape
  resizeMode: InferenceResizeMode
  rect: NormalizedRect
  polygon: NormalizedPoint[]
}

export const clamp = (value: number, low: number, high: number) => Math.max(low, Math.min(high, value))

export function inferenceRoiConfig(value: unknown): InferenceRoiValue | null {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return null
  const config = value as Record<string, unknown>
  if (!['roi_only', 'full_plus_roi', 'full_frame_roi_filter'].includes(String(config.mode))
      || !Array.isArray(config.polygon)) return null
  const resizeMode: InferenceResizeMode = ['stretch', 'expand', 'letterbox'].includes(String(config.resize_mode))
    ? config.resize_mode as InferenceResizeMode : 'stretch'
  const polygon: NormalizedPoint[] = config.polygon
    .filter((point): point is number[] => Array.isArray(point) && point.length === 2)
    .map(point => [Number(point[0]), Number(point[1])] as NormalizedPoint)
    .filter(point => point.every(Number.isFinite) && point[0] >= 0 && point[0] <= 1 && point[1] >= 0 && point[1] <= 1)
  if (polygon.length !== config.polygon.length || polygon.length < 3 || polygon.length > 64
      || polygonArea(polygon) <= 1e-8 || !polygonIsSimple(polygon)) return null
  const rect = regionBounds(polygon)
  const shape: InferenceRoiShape = isAxisAlignedRectangle(polygon, rect) ? 'rect' : 'polygon'
  return { mode: config.mode as InferenceRoiMode, shape, resizeMode, rect, polygon }
}

export const inferenceRoiRect = (value: unknown): NormalizedRect | null => inferenceRoiConfig(value)?.rect ?? null
export const inferenceRoiRestrictsResults = (mode: InferenceRoiMode): boolean =>
  mode === 'roi_only' || mode === 'full_frame_roi_filter'

export function inferenceRoiContainsPoint(config: InferenceRoiValue, x: number, y: number): boolean {
  return normalizedPointInPolygon(x, y, config.polygon)
}

export function normalizedPointInPolygon(x: number, y: number, polygon: NormalizedPoint[]): boolean {
  let inside = false
  for (let i = 0, j = polygon.length - 1; i < polygon.length; j = i++) {
    const [xi, yi] = polygon[i], [xj, yj] = polygon[j]
    const cross = (x - xi) * (yj - yi) - (y - yi) * (xj - xi)
    if (Math.abs(cross) <= 1e-9 && x >= Math.min(xi, xj) - 1e-9 && x <= Math.max(xi, xj) + 1e-9
        && y >= Math.min(yi, yj) - 1e-9 && y <= Math.max(yi, yj) + 1e-9) return true
    if ((yi > y) !== (yj > y) && x < ((xj - xi) * (y - yi)) / (yj - yi) + xi) inside = !inside
  }
  return inside
}

/** 判断整条业务 ROI 边界都位于推理多边形内；仅检查顶点会漏掉凹口穿越。 */
export function inferenceRoiContainsPolygon(config: InferenceRoiValue, polygon: NormalizedPoint[]): boolean {
  if (polygon.some(([x, y]) => !inferenceRoiContainsPoint(config, x, y))) return false
  const cross = (ax: number, ay: number, bx: number, by: number) => ax * by - ay * bx
  for (let index = 0; index < polygon.length; index++) {
    const a = polygon[index], b = polygon[(index + 1) % polygon.length]
    const rx = b[0] - a[0], ry = b[1] - a[1]
    const parameters = [0, 1]
    for (let edge = 0; edge < config.polygon.length; edge++) {
      const c = config.polygon[edge], d = config.polygon[(edge + 1) % config.polygon.length]
      const sx = d[0] - c[0], sy = d[1] - c[1]
      const denominator = cross(rx, ry, sx, sy)
      const qx = c[0] - a[0], qy = c[1] - a[1]
      if (Math.abs(denominator) > 1e-12) {
        const t = cross(qx, qy, sx, sy) / denominator
        const u = cross(qx, qy, rx, ry) / denominator
        if (t >= -1e-10 && t <= 1 + 1e-10 && u >= -1e-10 && u <= 1 + 1e-10)
          parameters.push(clamp(t, 0, 1))
      } else if (Math.abs(cross(qx, qy, rx, ry)) <= 1e-12) {
        const lengthSquared = rx * rx + ry * ry
        if (lengthSquared > 1e-16) {
          for (const point of [c, d]) {
            const t = ((point[0] - a[0]) * rx + (point[1] - a[1]) * ry) / lengthSquared
            if (t >= -1e-10 && t <= 1 + 1e-10) parameters.push(clamp(t, 0, 1))
          }
        }
      }
    }
    parameters.sort((x, y) => x - y)
    for (let i = 1; i < parameters.length; i++) {
      if (parameters[i] - parameters[i - 1] <= 1e-10) continue
      const t = (parameters[i] + parameters[i - 1]) / 2
      if (!normalizedPointInPolygon(a[0] + rx * t, a[1] + ry * t, config.polygon)) return false
    }
  }
  return true
}

export function regionBounds(polygon: NormalizedPoint[]): NormalizedRect {
  const xs = polygon.map(point => point[0]), ys = polygon.map(point => point[1])
  const x = Math.min(...xs), y = Math.min(...ys)
  return { x, y, width: Math.max(...xs) - x, height: Math.max(...ys) - y }
}

export function rectanglePolygon(rect: NormalizedRect): NormalizedPoint[] {
  return [[rect.x, rect.y], [rect.x + rect.width, rect.y],
    [rect.x + rect.width, rect.y + rect.height], [rect.x, rect.y + rect.height]]
}

function isAxisAlignedRectangle(polygon: NormalizedPoint[], bounds = regionBounds(polygon)): boolean {
  if (polygon.length !== 4 || bounds.width <= 0 || bounds.height <= 0) return false
  const right = bounds.x + bounds.width, bottom = bounds.y + bounds.height
  const corners = new Set<string>()
  for (const [x, y] of polygon) {
    const sideX = Math.abs(x - bounds.x) <= 1e-9 ? 0 : Math.abs(x - right) <= 1e-9 ? 1 : -1
    const sideY = Math.abs(y - bounds.y) <= 1e-9 ? 0 : Math.abs(y - bottom) <= 1e-9 ? 1 : -1
    if (sideX < 0 || sideY < 0) return false
    corners.add(`${sideX}:${sideY}`)
  }
  return corners.size === 4
}

export function polygonArea(polygon: NormalizedPoint[]): number {
  return Math.abs(polygon.reduce((sum, point, index) => {
    const next = polygon[(index + 1) % polygon.length]
    return sum + point[0] * next[1] - next[0] * point[1]
  }, 0)) / 2
}

export function polygonIsSimple(polygon: NormalizedPoint[]): boolean {
  const orientation = (a: NormalizedPoint, b: NormalizedPoint, c: NormalizedPoint) =>
    (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
  const onSegment = (p: NormalizedPoint, a: NormalizedPoint, b: NormalizedPoint) =>
    Math.abs(orientation(a, b, p)) <= 1e-10
      && p[0] >= Math.min(a[0], b[0]) - 1e-10 && p[0] <= Math.max(a[0], b[0]) + 1e-10
      && p[1] >= Math.min(a[1], b[1]) - 1e-10 && p[1] <= Math.max(a[1], b[1]) + 1e-10
  const intersects = (a: NormalizedPoint, b: NormalizedPoint, c: NormalizedPoint, d: NormalizedPoint) => {
    const o1 = orientation(a, b, c), o2 = orientation(a, b, d)
    const o3 = orientation(c, d, a), o4 = orientation(c, d, b)
    if (((o1 > 1e-10 && o2 < -1e-10) || (o1 < -1e-10 && o2 > 1e-10))
        && ((o3 > 1e-10 && o4 < -1e-10) || (o3 < -1e-10 && o4 > 1e-10))) return true
    return onSegment(c, a, b) || onSegment(d, a, b) || onSegment(a, c, d) || onSegment(b, c, d)
  }
  for (let i = 0; i < polygon.length; i++) {
    const next = polygon[(i + 1) % polygon.length]
    if (Math.hypot(polygon[i][0] - next[0], polygon[i][1] - next[1]) <= 1e-10) return false
    for (let j = i + 1; j < polygon.length; j++) {
      if (j === i + 1 || (i === 0 && j === polygon.length - 1)) continue
      if (intersects(polygon[i], next, polygon[j], polygon[(j + 1) % polygon.length])) return false
    }
  }
  return true
}

export function serializeInferenceRoi(config: InferenceRoiValue): Record<string, unknown> {
  const polygon = config.shape === 'rect' ? rectanglePolygon(config.rect) : config.polygon
  return {
    mode: config.mode,
    resize_mode: config.resizeMode,
    polygon: polygon.map(point => point.map(value => +value.toFixed(6))),
  }
}

