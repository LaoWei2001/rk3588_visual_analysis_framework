let texture: string | undefined

/** Fine bipolar signal noise, with occasional short scan-direction impulses.
 * Generate once; CSS repositions/flips the cached texture without frame readbacks.
 */
export function terminalFineSnowTexture(): string {
  if (texture) return texture
  const size = 512, canvas = document.createElement('canvas')
  canvas.width = canvas.height = size
  const context = canvas.getContext('2d')!
  const pixels = context.createImageData(size, size)
  let seed = 0x6d2b79f5
  const random = () => {
    seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5
    return (seed >>> 0) / 0x100000000
  }
  for (let y = 0; y < size; y++) {
    const gain = .85 + random() * .3
    let impulse = 0, tail = 0
    for (let x = 0; x < size; x++) {
      // Most grains sit near zero; both dark and light excursions avoid a white
      // sand-like veil. A slight horizontal correlation follows the scan direction.
      const sample = (random() + random() + random() - 1.5) * .55
      if (random() < .003) { impulse = random() > .5 ? .9 : -.9; tail = 2 + Math.floor(random() * 3) }
      const value = sample * gain + (tail > 0 ? impulse : 0)
      if (tail > 0) { tail--; impulse *= .55 }
      const i = (y * size + x) * 4
      pixels.data[i] = pixels.data[i + 1] = pixels.data[i + 2] = value > 0 ? 255 : 0
      pixels.data[i + 3] = Math.round(Math.min(.9, Math.abs(value)) * 255)
    }
  }
  context.putImageData(pixels, 0, 0)
  texture = `url("${canvas.toDataURL()}")`
  return texture
}
