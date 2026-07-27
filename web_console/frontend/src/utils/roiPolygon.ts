const SAME_POINT_EPSILON = 1e-6

function samePoint(a: number[], b: number[]): boolean {
  return Math.abs(a[0] - b[0]) <= SAME_POINT_EPSILON &&
    Math.abs(a[1] - b[1]) <= SAME_POINT_EPSILON
}

/**
 * ROI 多边形只保存互不重复的实际顶点。多边形在绘制/命中测试时会自动闭合，
 * 因此旧配置中用于表示闭合的末尾首点副本需要移除。
 */
export function normalizeRoiPolygon(polygon: number[][]): number[][] {
  const points = polygon
    .filter(point => Array.isArray(point) && point.length >= 2)
    .map(point => [Number(point[0]), Number(point[1])])
    .filter(point => Number.isFinite(point[0]) && Number.isFinite(point[1]))

  while (points.length > 1 && samePoint(points[0], points[points.length - 1])) {
    points.pop()
  }
  return points
}
