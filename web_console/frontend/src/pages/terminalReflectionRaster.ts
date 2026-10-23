/** Batch the existing reflection slices into two textured meshes. */
export type ReflectionSlice = [number, number, number, number, number, number, number, number]

export function createReflectionRaster() {
  const canvas = document.createElement('canvas')
  const gl = canvas.getContext('webgl', {
    alpha: true, premultipliedAlpha: true, antialias: false, depth: false,
    stencil: false, preserveDrawingBuffer: true, powerPreference: 'low-power',
  })
  if (!gl) return undefined
  const shader = (type: number, source: string) => {
    const value = gl.createShader(type)!
    gl.shaderSource(value, source); gl.compileShader(value)
    if (!gl.getShaderParameter(value, gl.COMPILE_STATUS)) { gl.deleteShader(value); return undefined }
    return value
  }
  const vertex = shader(gl.VERTEX_SHADER, `
    attribute vec2 position; attribute vec2 coordinate; varying vec2 uv;
    void main(){gl_Position=vec4(position,0.,1.);uv=coordinate;}`)
  const fragment = shader(gl.FRAGMENT_SHADER, `
    precision highp float; varying vec2 uv; uniform sampler2D light;
    void main(){
      if(uv.x<0. || uv.x>1. || uv.y<0. || uv.y>1.) discard;
      gl_FragColor=texture2D(light,uv);
    }`)
  if (!vertex || !fragment) { gl.getExtension('WEBGL_lose_context')?.loseContext(); return undefined }
  const program = gl.createProgram()!
  gl.attachShader(program, vertex); gl.attachShader(program, fragment); gl.linkProgram(program)
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) { gl.getExtension('WEBGL_lose_context')?.loseContext(); return undefined }
  gl.deleteShader(vertex); gl.deleteShader(fragment)
  gl.useProgram(program)
  const position = gl.getAttribLocation(program, 'position'), coordinate = gl.getAttribLocation(program, 'coordinate')
  gl.enableVertexAttribArray(position); gl.enableVertexAttribArray(coordinate)
  const texture = gl.createTexture()!
  gl.bindTexture(gl.TEXTURE_2D, texture)
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR)
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR)
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE)
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE)
  gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, true)
  gl.enable(gl.BLEND); gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA)
  let buffers: { buffer: WebGLBuffer; count: number }[] = []
  const configure = (groups: ReflectionSlice[][], w: number, h: number, sw: number, sh: number) => {
    for (const item of buffers) gl.deleteBuffer(item.buffer)
    canvas.width = w; canvas.height = h
    gl.viewport(0, 0, w, h)
    buffers = groups.map((slices, axis) => {
      const data: number[] = []
      const point = (x: number, y: number, u: number, v: number) => data.push(x / w * 2 - 1, 1 - y / h * 2, u / sw, v / sh)
      for (const [sx, sy, sourceWidth, sourceHeight, dx, dy, dw, dh] of slices) {
        const x0 = axis ? -dx : dx, y0 = axis ? dy : -dy
        const x1 = axis ? -dx - dw : dx + dw, y1 = axis ? dy + dh : -dy - dh
        point(x0,y0,sx,sy);point(x1,y0,sx+sourceWidth,sy);point(x0,y1,sx,sy+sourceHeight)
        point(x0,y1,sx,sy+sourceHeight);point(x1,y0,sx+sourceWidth,sy);point(x1,y1,sx+sourceWidth,sy+sourceHeight)
      }
      const buffer = gl.createBuffer()!
      gl.bindBuffer(gl.ARRAY_BUFFER, buffer); gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(data), gl.STATIC_DRAW)
      return { buffer, count: data.length / 4 }
    })
  }
  return {
    configure,
    upload(source: HTMLCanvasElement) {
      if (gl.isContextLost()) return false
      gl.bindTexture(gl.TEXTURE_2D, texture)
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, source)
      return true
    },
    draw(group: number) {
      const item = buffers[group]
      gl.clear(gl.COLOR_BUFFER_BIT)
      gl.bindBuffer(gl.ARRAY_BUFFER, item.buffer)
      gl.vertexAttribPointer(position, 2, gl.FLOAT, false, 16, 0)
      gl.vertexAttribPointer(coordinate, 2, gl.FLOAT, false, 16, 8)
      gl.drawArrays(gl.TRIANGLES, 0, item.count)
      return canvas
    },
    dispose() {
      for (const item of buffers) gl.deleteBuffer(item.buffer)
      gl.deleteTexture(texture); gl.deleteProgram(program)
      gl.getExtension('WEBGL_lose_context')?.loseContext()
      canvas.width = 0; canvas.height = 0
    },
  }
}
