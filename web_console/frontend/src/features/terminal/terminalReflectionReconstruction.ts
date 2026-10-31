/** Reconstruct linear radiance within each wall; never blur across a joint. */
export const RECONSTRUCTION_RADIUS = 3
export const RECONSTRUCTION_SIGMA = 1.35
export function reflectionReconstruction(width: number, height: number, positions: Uint32Array, faces: Uint8Array, radius = RECONSTRUCTION_RADIUS, sigma = RECONSTRUCTION_SIGMA) {
  const offsets = new Uint32Array(positions.length + 1)
  const neighbors: number[] = [], weights: number[] = []
  for (let i = 0; i < positions.length; i++) {
    const p = positions[i], x = p % width, y = Math.floor(p / width), begin = neighbors.length
    let total = 0
    for (let dy = -radius; dy <= radius; dy++) {
      for (let dx = -radius; dx <= radius; dx++) {
        const xx = x + dx, yy = y + dy, q = yy * width + xx
        if (xx < 0 || xx >= width || yy < 0 || yy >= height || faces[q] !== faces[p]) continue
        const weight = Math.exp(-(dx * dx + dy * dy) / (2 * sigma ** 2))
        neighbors.push(q * 3); weights.push(weight); total += weight
      }
    }
    for (let j = begin; j < weights.length; j++) weights[j] /= total
    offsets[i + 1] = neighbors.length
  }
  return { offsets, neighbors: new Uint32Array(neighbors), weights: new Float32Array(weights) }
}
