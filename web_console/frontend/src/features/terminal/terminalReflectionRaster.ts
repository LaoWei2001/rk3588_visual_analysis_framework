import { reflectionOverlay, linearLight } from './terminalReflectionComposite'
import { LOBE_SAMPLES, TRANSPORT_SAMPLES } from './terminalReflectionTransport'
import { reflectionReconstruction, RECONSTRUCTION_RADIUS, RECONSTRUCTION_SIGMA } from './terminalReflectionReconstruction'
import type { ReflectionTransport } from './terminalReflectionTransport'

const linear = (v: number) => v <= .04045 ? v / 12.92 : ((v + .055) / 1.055) ** 2.4
const srgb = (v: number) => v <= .0031308 ? v * 12.92 : 1.055 * v ** (1 / 2.4) - .055
const decode = Float32Array.from({ length: 4096 }, (_, i) => linear(i / 4095))

/** Cached sparse light paths, evaluated only when emitted screen light changes.
 * The GPU performs texture lookups, not per-frame ray tracing or canvas readback.
 * The software path evaluates exactly the same transport when WebGL is absent.
 */
export function createReflectionRaster() {
  const canvas = document.createElement('canvas')
  const gl = canvas.getContext('webgl', {
    alpha: true, premultipliedAlpha: true, antialias: false,
    depth: false, stencil: false, preserveDrawingBuffer: true, powerPreference: 'low-power'
  })
  let transport: ReflectionTransport
  const software = document.createElement('canvas'), cpu = software.getContext('2d')!
  let reconstruction: ReturnType<typeof reflectionReconstruction> | undefined
  let output: ImageData
  let softwareKey = '', offsets = new Uint32Array(), coefficients = new Float32Array()
  let accumulated = new Float32Array()
  let gpu: ReturnType<typeof createGPU> | undefined
  if (gl) gpu = createGPU(gl, canvas)
  return {
    configure(next: ReflectionTransport) {
      transport = next
      reconstruction = undefined
      software.width = next.width; software.height = next.height
      output = cpu.createImageData(next.width, next.height)
      softwareKey = ''; accumulated = new Float32Array(next.width * next.height * 3)
      gpu?.configure(next)
    },
    draw(source: HTMLCanvasElement) {
      if (gpu?.draw(source)) return canvas
      const sw = source.width, sh = source.height
      const pixels = source.getContext('2d')!.getImageData(0, 0, sw, sh).data
      output.data.fill(0); accumulated.fill(0)
      const { positions, paths } = transport, samples = transport.samples ?? TRANSPORT_SAMPLES
      reconstruction ??= reflectionReconstruction(transport.width, transport.height, positions, transport.faces, transport.radius, transport.sigma)
      if (softwareKey !== `${sw}:${sh}`) {
        const ids: number[] = [], weights: number[] = []
        for (let i = 0; i < positions.length; i++) for (let j = 0; j < samples; j++) {
          const p = (i * samples + j) * 3, weight = paths[p + 2]
          if (!weight) continue
          const x = Math.max(0, Math.min(sw - 1, paths[p] * sw - .5)), y = Math.max(0, Math.min(sh - 1, paths[p + 1] * sh - .5))
          const x0 = Math.floor(x), y0 = Math.floor(y), x1 = Math.min(sw - 1, x0 + 1), y1 = Math.min(sh - 1, y0 + 1)
          const fx = x - x0, fy = y - y0
          ids.push(positions[i] * 3, (y0 * sw + x0) * 4, (y0 * sw + x1) * 4, (y1 * sw + x0) * 4, (y1 * sw + x1) * 4)
          weights.push(weight, (1 - fx) * (1 - fy) / 255, fx * (1 - fy) / 255, (1 - fx) * fy / 255, fx * fy / 255)
        }
        offsets = new Uint32Array(ids); coefficients = new Float32Array(weights); softwareKey = `${sw}:${sh}`
      }
      // Source dimensions are stable: even the fallback's bilinear footprints
      // are cached. Per-frame work is only gathering and accumulating radiance.
      for (let i = 0; i < offsets.length; i += 5) {
        const p0 = offsets[i + 1], p1 = offsets[i + 2], p2 = offsets[i + 3], p3 = offsets[i + 4], out = offsets[i]
        const a0 = pixels[p0 + 3] * coefficients[i + 1], a1 = pixels[p1 + 3] * coefficients[i + 2]
        const a2 = pixels[p2 + 3] * coefficients[i + 3], a3 = pixels[p3 + 3] * coefficients[i + 4], alpha = a0 + a1 + a2 + a3
        if (!alpha) continue
        const factor = 4095 / (255 * alpha), throughput = coefficients[i] * alpha
        for (let c = 0; c < 3; c++) accumulated[out + c] += decode[Math.min(4095, Math.round(
          (pixels[p0 + c] * a0 + pixels[p1 + c] * a1 + pixels[p2 + c] * a2 + pixels[p3 + c] * a3) * factor))] * throughput
      }
      for (let i = 0; i < positions.length; i++) {
        const sum = [0, 0, 0]
        for (let j = reconstruction.offsets[i]; j < reconstruction.offsets[i + 1]; j++) {
          const at = reconstruction.neighbors[j], weight = reconstruction.weights[j]
          for (let c = 0; c < 3; c++) sum[c] += accumulated[at + c] * weight
        }
        const color = sum.map(srgb), alpha = Math.min(1, Math.max(...color)), offset = positions[i] * 4
        if (!alpha) continue
        for (let c = 0; c < 3; c++) output.data[offset + c] = Math.round(color[c] / alpha * 255)
        output.data[offset + 3] = Math.round(alpha * 255)
      }
      cpu.putImageData(output, 0, 0)
      return software
    },
    composite(source: HTMLCanvasElement, strength: number) {
      if (gpu?.composite(source, strength)) return canvas
      if (source !== software) {
        cpu.clearRect(0, 0, software.width, software.height)
        cpu.drawImage(source, 0, 0, software.width, software.height)
      }
      const pixels = cpu.getImageData(0, 0, software.width, software.height).data
      output.data.fill(0)
      for (const p of transport.positions) {
        const at = p * 4, a = pixels[at + 3] / 255
        const base = [0, 1, 2].map(c => (transport.base?.[at + c] ?? 0) / 255)
        const radiance = [0, 1, 2].map(c => linearLight(pixels[at + c] / 255 * a))
        const result = reflectionOverlay(base, radiance, strength), alpha = result[3]
        if (!alpha) continue
        for (let c = 0; c < 3; c++)output.data[at + c] = Math.round(result[c] / alpha * 255)
        output.data[at + 3] = Math.round(alpha * 255)
      }
      cpu.putImageData(output, 0, 0)
      return software
    },
    dispose() { gpu?.dispose(); software.width = software.height = canvas.width = canvas.height = 0 },
  }
}

function createGPU(gl: WebGLRenderingContext, canvas: HTMLCanvasElement) {
  const shader = (type: number, source: string) => {
    const s = gl.createShader(type)!
    gl.shaderSource(s, source); gl.compileShader(s)
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) { gl.deleteShader(s); return undefined }
    return s
  }
  const vertex = shader(gl.VERTEX_SHADER, `
    attribute vec2 position; attribute vec2 pathBase; varying vec2 base;
    void main(){gl_Position=vec4(position,0.,1.);gl_PointSize=1.;base=pathBase;}`)
  const fragment = shader(gl.FRAGMENT_SHADER, `
    precision highp float;
    varying vec2 base; uniform sampler2D light; uniform sampler2D paths;
    uniform sampler2D weights; uniform vec2 atlasSize; uniform float pathCount; uniform float weightScale;
    vec3 linear(vec3 c){return mix(c/12.92,pow((c+.055)/1.055,vec3(2.4)),step(vec3(.04045),c));}
    vec3 srgb(vec3 c){return mix(c*12.92,1.055*pow(c,vec3(1./2.4))-.055,step(vec3(.0031308),c));}
    void main(){
      vec3 sum=vec3(0.);
      for(int j=0;j<${TRANSPORT_SAMPLES};j++){
        if(float(j)>=pathCount)break;
        vec2 at=(base+vec2(float(j)+.5,.5))/atlasSize;
        float weight=texture2D(weights,at).r/weightScale;
        if(weight>0.){
          vec4 encodedUV=texture2D(paths,at);
          vec2 uv=vec2(dot(encodedUV.rg,vec2(65280.,255.)),dot(encodedUV.ba,vec2(65280.,255.)))/65535.;
          vec4 emitted=texture2D(light,uv);
          sum+=linear(emitted.rgb/max(emitted.a,.00001))*emitted.a*weight;
        }
      }
      vec3 color=srgb(sum);
      gl_FragColor=vec4(color,max(color.r,max(color.g,color.b)));
    }`)
  if (!vertex || !fragment) { gl.getExtension('WEBGL_lose_context')?.loseContext(); return undefined }
  const program = gl.createProgram()!
  gl.attachShader(program, vertex); gl.attachShader(program, fragment)
  gl.bindAttribLocation(program, 0, 'position'); gl.bindAttribLocation(program, 1, 'pathBase'); gl.linkProgram(program)
  gl.deleteShader(vertex); gl.deleteShader(fragment)
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) { gl.deleteProgram(program); gl.getExtension('WEBGL_lose_context')?.loseContext(); return undefined }
  const smoothVertex = shader(gl.VERTEX_SHADER, `
    attribute vec2 position; varying vec2 uv;
    void main(){gl_Position=vec4(position,0.,1.);gl_PointSize=1.;uv=vec2(position.x+1.,1.-position.y)*.5;}`)
  const smoothFragment = shader(gl.FRAGMENT_SHADER, `
    precision highp float; varying vec2 uv;
    uniform sampler2D field; uniform sampler2D faces; uniform vec2 fieldSize; uniform float radius; uniform float sigma;
    vec3 linear(vec3 c){return mix(c/12.92,pow((c+.055)/1.055,vec3(2.4)),step(vec3(.04045),c));}
    vec3 srgb(vec3 c){return mix(c*12.92,1.055*pow(c,vec3(1./2.4))-.055,step(vec3(.0031308),c));}
    void main(){
      float face=texture2D(faces,uv).r,total=0.;vec3 sum=vec3(0.);
      for(int y=-${RECONSTRUCTION_RADIUS};y<=${RECONSTRUCTION_RADIUS};y++){
        for(int x=-${RECONSTRUCTION_RADIUS};x<=${RECONSTRUCTION_RADIUS};x++){
          if(abs(float(x))>radius||abs(float(y))>radius)continue;
          vec2 at=uv+vec2(float(x),float(y))/fieldSize;
          if(at.x>0.&&at.x<1.&&at.y>0.&&at.y<1.&&abs(texture2D(faces,at).r-face)<.001){
            float weight=exp(-float(x*x+y*y)/(2.*sigma*sigma));
            // Decode premultiplied sRGB radiance before averaging.
            vec3 radiance=linear(texture2D(field,vec2(at.x,1.-at.y)).rgb);
            sum+=radiance*weight;total+=weight;
          }
        }
      }
      vec3 color=srgb(sum/max(total,.00001));
      gl_FragColor=vec4(color,max(color.r,max(color.g,color.b)));
    }`)
  if (!smoothVertex || !smoothFragment) { gl.getExtension('WEBGL_lose_context')?.loseContext(); return undefined }
  const smooth = gl.createProgram()!
  gl.attachShader(smooth, smoothVertex); gl.attachShader(smooth, smoothFragment)
  gl.bindAttribLocation(smooth, 0, 'position'); gl.linkProgram(smooth)
  gl.deleteShader(smoothVertex); gl.deleteShader(smoothFragment)
  if (!gl.getProgramParameter(smooth, gl.LINK_STATUS)) { gl.getExtension('WEBGL_lose_context')?.loseContext(); return undefined }
  const compositeVertex = shader(gl.VERTEX_SHADER, `
    attribute vec2 position;varying vec2 uv;
    void main(){gl_Position=vec4(position,0.,1.);gl_PointSize=1.;uv=vec2(position.x+1.,1.-position.y)*.5;}`)
  const compositeFragment = shader(gl.FRAGMENT_SHADER, `
    precision highp float;varying vec2 uv;
    uniform sampler2D reflected;uniform sampler2D material;uniform float strength;
    vec3 linear(vec3 c){return mix(c/12.92,pow((c+.055)/1.055,vec3(2.4)),step(vec3(.04045),c));}
    vec3 srgb(vec3 c){return mix(c*12.92,1.055*pow(c,vec3(1./2.4))-.055,step(vec3(.0031308),c));}
    void main(){
      vec3 base=texture2D(material,uv).rgb;
      vec3 radiance=linear(texture2D(reflected,uv).rgb);
      vec3 delta=max(vec3(0.),min(vec3(1.),srgb(linear(base)+radiance*strength))-base);
      vec3 coverage=delta/max(vec3(.000001),vec3(1.)-base);
      float alpha=max(coverage.r,max(coverage.g,coverage.b));
      gl_FragColor=vec4(delta+base*alpha,alpha);
    }`)
  if (!compositeVertex || !compositeFragment) { gl.getExtension('WEBGL_lose_context')?.loseContext(); return undefined }
  const composite = gl.createProgram()!
  gl.attachShader(composite, compositeVertex); gl.attachShader(composite, compositeFragment)
  gl.bindAttribLocation(composite, 0, 'position'); gl.linkProgram(composite)
  gl.deleteShader(compositeVertex); gl.deleteShader(compositeFragment)
  if (!gl.getProgramParameter(composite, gl.LINK_STATUS)) { gl.getExtension('WEBGL_lose_context')?.loseContext(); return undefined }
  const material = gl.createTexture()!
  const field = gl.createTexture()!, faces = gl.createTexture()!, framebuffer = gl.createFramebuffer()!
  const nearest = (texture: WebGLTexture, unit: number) => {
    gl.activeTexture(gl.TEXTURE0 + unit); gl.bindTexture(gl.TEXTURE_2D, texture)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE)
  }
  nearest(field, 3); nearest(faces, 4); nearest(material, 5)
  gl.useProgram(composite)
  gl.uniform1i(gl.getUniformLocation(composite, 'reflected'), 0); gl.uniform1i(gl.getUniformLocation(composite, 'material'), 5)
  gl.useProgram(smooth)
  gl.uniform1i(gl.getUniformLocation(smooth, 'field'), 3); gl.uniform1i(gl.getUniformLocation(smooth, 'faces'), 4)
  gl.useProgram(program)
  const textures = [0, 1, 2].map((unit) => {
    const t = gl.createTexture()!
    gl.activeTexture(gl.TEXTURE0 + unit); gl.bindTexture(gl.TEXTURE_2D, t)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, unit ? gl.NEAREST : gl.LINEAR)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, unit ? gl.NEAREST : gl.LINEAR)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE)
    return t
  })
  for (const [unit, name] of ['light', 'paths', 'weights'].entries()) gl.uniform1i(gl.getUniformLocation(program, name), unit)
  const buffer = gl.createBuffer()!
  const position = gl.getAttribLocation(program, 'position'), pathBase = gl.getAttribLocation(program, 'pathBase')
  gl.enableVertexAttribArray(position); gl.enableVertexAttribArray(pathBase)
  let count = 0, valid = false, reconstruct = true
  return {
    configure(map: ReflectionTransport) {
      const { width, height, positions, paths } = map, samples = map.samples ?? TRANSPORT_SAMPLES, weightScale = map.weightScale ?? LOBE_SAMPLES
      reconstruct = (map.radius ?? RECONSTRUCTION_RADIUS) > 0
      canvas.width = width; canvas.height = height; gl.viewport(0, 0, width, height)
      gl.useProgram(smooth); gl.uniform2f(gl.getUniformLocation(smooth, 'fieldSize'), width, height)
      gl.uniform1f(gl.getUniformLocation(smooth, 'radius'), map.radius ?? RECONSTRUCTION_RADIUS)
      gl.uniform1f(gl.getUniformLocation(smooth, 'sigma'), map.sigma ?? RECONSTRUCTION_SIGMA)
      gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1)
      nearest(field, 3); gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, width, height, 0, gl.RGBA, gl.UNSIGNED_BYTE, null)
      gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer)
      gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, field, 0)
      const complete = gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE
      gl.bindFramebuffer(gl.FRAMEBUFFER, null)
      nearest(faces, 4); gl.texImage2D(gl.TEXTURE_2D, 0, gl.LUMINANCE, width, height, 0, gl.LUMINANCE, gl.UNSIGNED_BYTE, map.faces)
      nearest(material, 5)
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, width, height, 0, gl.RGBA, gl.UNSIGNED_BYTE, map.base ?? new Uint8Array(width * height * 4))
      gl.useProgram(program)
      count = positions.length
      const perRow = Math.min(64, Math.floor(gl.getParameter(gl.MAX_TEXTURE_SIZE) / samples))
      const aw = samples * perRow, ah = Math.max(1, Math.ceil(count / perRow))
      valid = complete && ah <= gl.getParameter(gl.MAX_TEXTURE_SIZE) && aw <= gl.getParameter(gl.MAX_TEXTURE_SIZE)
      if (!valid) return
      const coords = new Uint8Array(aw * ah * 4), weights = new Uint8Array(aw * ah), vertices = new Float32Array(count * 4)
      for (let i = 0; i < count; i++) {
        vertices.set([(positions[i] % width + .5) / width * 2 - 1, 1 - (Math.floor(positions[i] / width) + .5) / height * 2,
        i % perRow * samples, Math.floor(i / perRow)], i * 4)
        for (let j = 0; j < samples; j++) {
          const p = (i * samples + j) * 3, at = i * samples + j
          const u = Math.round(paths[p] * 65535), v = Math.round(paths[p + 1] * 65535)
          coords.set([u >>> 8, u & 255, v >>> 8, v & 255], at * 4)
          weights[at] = Math.round(paths[p + 2] * weightScale * 255)
        }
      }
      gl.bindBuffer(gl.ARRAY_BUFFER, buffer); gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.STATIC_DRAW)
      gl.vertexAttribPointer(position, 2, gl.FLOAT, false, 16, 0); gl.vertexAttribPointer(pathBase, 2, gl.FLOAT, false, 16, 8)
      gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false)
      gl.activeTexture(gl.TEXTURE1); gl.bindTexture(gl.TEXTURE_2D, textures[1])
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, aw, ah, 0, gl.RGBA, gl.UNSIGNED_BYTE, coords)
      gl.activeTexture(gl.TEXTURE2); gl.bindTexture(gl.TEXTURE_2D, textures[2])
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.LUMINANCE, aw, ah, 0, gl.LUMINANCE, gl.UNSIGNED_BYTE, weights)
      gl.uniform2f(gl.getUniformLocation(program, 'atlasSize'), aw, ah)
      gl.uniform1f(gl.getUniformLocation(program, 'pathCount'), samples)
      gl.uniform1f(gl.getUniformLocation(program, 'weightScale'), weightScale)
    },
    draw(source: HTMLCanvasElement) {
      if (!valid || gl.isContextLost()) return false
      gl.useProgram(program)
      gl.bindFramebuffer(gl.FRAMEBUFFER, reconstruct ? framebuffer : null)
      gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, textures[0])
      gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, true)
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, source)
      gl.clear(gl.COLOR_BUFFER_BIT); gl.drawArrays(gl.POINTS, 0, count)
      if (!reconstruct) return true
      gl.bindFramebuffer(gl.FRAMEBUFFER, null); gl.useProgram(smooth)
      gl.clear(gl.COLOR_BUFFER_BIT); gl.drawArrays(gl.POINTS, 0, count)
      return true
    },
    composite(source: HTMLCanvasElement, strength: number) {
      if (!valid || gl.isContextLost()) return false
      gl.useProgram(composite); gl.bindFramebuffer(gl.FRAMEBUFFER, null)
      gl.uniform1f(gl.getUniformLocation(composite, 'strength'), strength)
      gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, textures[0])
      gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, true)
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, source)
      gl.clear(gl.COLOR_BUFFER_BIT); gl.drawArrays(gl.POINTS, 0, count)
      return true
    },
    dispose() {
      gl.deleteProgram(composite); gl.deleteTexture(material)
      gl.deleteBuffer(buffer); textures.forEach(t => gl.deleteTexture(t)); gl.deleteProgram(program)
      gl.deleteProgram(smooth); gl.deleteTexture(field); gl.deleteTexture(faces); gl.deleteFramebuffer(framebuffer)
      gl.getExtension('WEBGL_lose_context')?.loseContext()
    },
  }
}
