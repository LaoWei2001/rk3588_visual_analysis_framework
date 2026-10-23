export type BezelFace = 'top' | 'right' | 'bottom' | 'left'
type Point = [number, number]

// Projected setback of the glass behind the front opening, independent of curvature.
export const SCREEN_RECESS = 14
export const BEZEL_JOINT_WIDTH = 2

/** Outer corner to actual curved-glass corner, including the screen setback. */
export function bezelJoints(width: number, height: number, amount: number, inset: number): [Point, Point][] {
  // At a glass corner both source coordinates are +/-1: t + amount*t^3 = 1.
  let t = 1
  for (let i = 0; i < 8; i++) t -= (t + amount * t ** 3 - 1) / (1 + 3 * amount * t * t)
  const x = inset + (width - inset * 2) * (1 - t) / 2
  const y = inset + (height - inset * 2) * (1 - t) / 2
  return [
    [[0, 0], [x, y]], [[width, 0], [width - x, y]],
    [[width, height], [width - x, height - y]], [[0, height], [x, height - y]],
  ]
}

/** Material and light meet at the same four mitres, ending at the glass corners. */
export function bezelFaces(width: number, height: number, amount: number, inset: number): Record<BezelFace, Point[]> {
  const joints = bezelJoints(width, height, amount, inset)
  const center: Point = [width / 2, height / 2]
  const face = (a: number, b: number) => [joints[a][0], joints[b][0], joints[b][1], center, joints[a][1]]
  return { top: face(0, 1), right: face(1, 2), bottom: face(2, 3), left: face(3, 0) }
}
