// One first-level directory is one font choice. Only URLs enter the bundle;
// FontFace loads the selected family, never the entire font library.
const fontFiles = import.meta.glob<string>([
  '../assets/fonts/*/**/*.{woff2,woff,ttf,otf}',
  '../assets/fonts/*/**/*.{WOFF2,WOFF,TTF,OTF}',
], { eager: true, query: '?url&no-inline', import: 'default' })
const textFiles = import.meta.glob<string>([
  '../assets/fonts/*/**/*.{txt,md}', '../assets/fonts/*/**/*.{TXT,MD}',
  '../assets/fonts/*/**/{OFL,LICENSE,LICENCE,COPYING,ofl,license,licence,copying}',
], { eager: true, query: '?url&no-inline', import: 'default' })

const PREFIX = '../assets/fonts/'
const FALLBACK = '"Noto Sans Mono CJK SC", "Microsoft YaHei", monospace'
const FORMATS: Record<string, string> = { woff2: 'woff2', woff: 'woff', ttf: 'truetype', otf: 'opentype' }
const FORMAT_ORDER: Record<string, number> = { woff2: 0, woff: 1, ttf: 2, otf: 3 }
export type TerminalFont = string
export interface TerminalFontDefinition {
  label: string
  faceFamily: string
  loadFamily: string
  family: string
  sources?: { url: string; format: string }[]
  license?: string
}
function relative(path: string) { return path.slice(PREFIX.length) }
function basename(path: string) { return path.slice(path.lastIndexOf('/') + 1) }
function extension(path: string) { return path.slice(path.lastIndexOf('.') + 1).toLowerCase() }
function styleRank(path: string) {
  const name = basename(path).replace(/\.[^.]+$/, '')
  if (/(regular|normal|book|roman)(?:[-_ ]|$)/i.test(name)) return 0
  return /(bold|italic|oblique|medium|light|thin|black)(?:[-_ ]|$)/i.test(name) ? 2 : 1
}
function fontOrder(a: string, b: string) {
  return styleRank(a) - styleRank(b) || FORMAT_ORDER[extension(a)] - FORMAT_ORDER[extension(b)] || a.localeCompare(b)
}
function licenseRank(path: string) {
  const name = basename(path)
  return /ofl/i.test(name) ? 0 : /licen[cs]e/i.test(name) ? 1 : 2
}
const folders = new Map<string, string[]>()
for (const path of Object.keys(fontFiles)) {
  const folder = relative(path).split('/')[0]
  const files = folders.get(folder) ?? []
  files.push(path); folders.set(folder, files)
}
export const TERMINAL_FONTS: Record<TerminalFont, TerminalFontDefinition> = Object.create(null)
for (const [folder, files] of [...folders].sort(([a], [b]) => a.localeCompare(b))) {
  files.sort(fontOrder)
  const bestStyle = styleRank(files[0])
  const licenses = Object.keys(textFiles).filter(path => relative(path).split('/')[0] === folder
    && /(ofl|licen[cs]e|copying)/i.test(basename(path)))
    .sort((a, b) => licenseRank(a) - licenseRank(b) || relative(a).split('/').length - relative(b).split('/').length || a.localeCompare(b))
  // Keep custom names out of the page's system font namespace. The code-point
  // suffix also distinguishes directories whose names differ only in case.
  const faceFamily = `RKConsoleTerminal_${folder}_${Array.from(folder, c => c.codePointAt(0)!.toString(16)).join('_')}`
  const loadFamily = JSON.stringify(faceFamily)
  TERMINAL_FONTS[`font:${folder}`] = {
    label: folder, faceFamily, loadFamily, family: `${loadFamily}, ${FALLBACK}`,
    sources: files.filter(path => styleRank(path) === bestStyle).map(path => ({ url: fontFiles[path], format: FORMATS[extension(path)] })),
    license: licenses[0] ? textFiles[licenses[0]] : undefined,
  }
}
TERMINAL_FONTS.modern = {
  label: '现代等宽', faceFamily: 'monospace', loadFamily: 'monospace',
  family: '"Cascadia Mono", "SFMono-Regular", Consolas, "Liberation Mono", monospace',
}
export const DEFAULT_TERMINAL_FONT = TERMINAL_FONTS['font:Departure Mono']
  ? 'font:Departure Mono' : Object.keys(TERMINAL_FONTS)[0]

/** Migrate saved choices from the previous fixed font list. */
export function resolveTerminalFont(value: unknown): TerminalFont {
  const legacy: Record<string, string> = { departure: 'font:Departure Mono', vt323: 'font:VT323' }
  const id = typeof value === 'string' ? legacy[value] ?? value : ''
  return Object.prototype.hasOwnProperty.call(TERMINAL_FONTS, id) ? id : DEFAULT_TERMINAL_FONT
}
const registered = new Map<TerminalFont, FontFace>()
export function loadTerminalFont(id: TerminalFont, size: number): Promise<FontFace[]> {
  const resolved = resolveTerminalFont(id)
  const font = TERMINAL_FONTS[resolved]
  if (font.sources && !registered.has(resolved)) {
    // FontFace accepts the raw family name; CSS load strings quote it separately.
    const source = font.sources.map(file => `url(${JSON.stringify(file.url)}) format(${JSON.stringify(file.format)})`).join(', ')
    const face = new FontFace(font.faceFamily, source, { style: 'normal', weight: '400', display: 'swap' })
    document.fonts.add(face); registered.set(resolved, face)
  }
  return document.fonts.load(`${size}px ${font.loadFamily}`)
}
