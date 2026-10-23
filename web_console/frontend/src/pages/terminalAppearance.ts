import type { ITheme, ITerminalOptions } from '@xterm/xterm'

// Inspired by https://github.com/Swordfish90/cool-retro-term.
// Independent browser implementation; bundled fonts retain their SIL OFL notices.
export type TerminalProfile = 'amber' | 'green' | 'white' | 'modern'

import { DEFAULT_TERMINAL_FONT, resolveTerminalFont } from './terminalFonts'
import type { TerminalFont } from './terminalFonts'
export { TERMINAL_FONTS } from './terminalFonts'
export type { TerminalFont } from './terminalFonts'

export interface TerminalAppearance {
  font: TerminalFont
  fontPreset: number
  horizontalSync: number
  rgbShift: number
  profile: TerminalProfile
  enabled: boolean
  brightness: number
  glow: number
  bloom: number
  flicker: number
  jitter: number
  pixelGrid: number
  pixelSize: number
  scanlines: number
  vignette: number
  noise: number
  curvature: number
  reflection: number
  afterglow: number
  sweep: boolean
}

export const TERMINAL_PROFILES: Record<TerminalProfile, {
  label: string; caption: string; foreground: string; background: string; rgb: string
}> = {
  amber: { label: '经典琥珀', caption: 'AMBER PHOSPHOR', foreground: '#ffbc69', background: '#140e08', rgb: '255, 188, 105' },
  green: { label: '荧光绿', caption: 'GREEN PHOSPHOR', foreground: '#80ef9b', background: '#08120c', rgb: '128, 239, 155' },
  white: { label: '白磷屏', caption: 'WHITE PHOSPHOR', foreground: '#d5e5ed', background: '#0d1116', rgb: '213, 229, 237' },
  modern: { label: '现代原色', caption: 'TRUE COLOR', foreground: '#cbd5e1', background: '#080b14', rgb: '148, 177, 226' },
}

export const DEFAULT_APPEARANCE: TerminalAppearance = {
  profile: 'amber', font: DEFAULT_TERMINAL_FONT, fontPreset: 1, pixelGrid: 70, pixelSize: 3, horizontalSync: 20, rgbShift: 20, enabled: true, brightness: 100, glow: 40, bloom: 35, flicker: 30, jitter: 35, scanlines: 24, vignette: 35, noise: 22, curvature: 45, reflection: 55, afterglow: 55, sweep: false,
}

const STORAGE_KEY = 'terminal-appearance-v1'

export function loadTerminalAppearance(): TerminalAppearance {
  try {
    const stored: unknown = JSON.parse(localStorage.getItem(STORAGE_KEY) ?? 'null')
    if (!stored || typeof stored !== 'object') return { ...DEFAULT_APPEARANCE }
    const value = stored as Record<string, unknown>
    const strength = (key: 'pixelGrid' | 'horizontalSync' | 'rgbShift' | 'glow' | 'bloom' | 'flicker' | 'jitter' | 'scanlines' | 'vignette' | 'noise' | 'curvature' | 'reflection' | 'afterglow') => {
      const n = value[key]
      return typeof n === 'number' && Number.isFinite(n)
        ? Math.min(100, Math.max(0, n)) : DEFAULT_APPEARANCE[key]
    }
    const savedFont = resolveTerminalFont(value.font)
    return {
      profile: typeof value.profile === 'string' && Object.prototype.hasOwnProperty.call(TERMINAL_PROFILES, value.profile)
        ? value.profile as TerminalProfile : DEFAULT_APPEARANCE.profile,
      // Upgrade the old default once. A later explicit VT323 choice is retained.
      font: !value.fontPreset && value.font === 'vt323' ? DEFAULT_TERMINAL_FONT : savedFont,
      fontPreset: 1,
      pixelGrid: strength('pixelGrid'),
      pixelSize: typeof value.pixelSize === 'number' && [2, 3, 4, 5, 6].includes(value.pixelSize)
        ? value.pixelSize : DEFAULT_APPEARANCE.pixelSize,
      horizontalSync: strength('horizontalSync'), rgbShift: strength('rgbShift'),
      enabled: typeof value.enabled === 'boolean' ? value.enabled : DEFAULT_APPEARANCE.enabled,
      brightness: typeof value.brightness === 'number' && Number.isFinite(value.brightness)
        ? Math.min(200, Math.max(0, value.brightness)) : DEFAULT_APPEARANCE.brightness,
      glow: strength('glow'), bloom: strength('bloom'), flicker: strength('flicker'), jitter: strength('jitter'), scanlines: strength('scanlines'),
      vignette: strength('vignette'), noise: strength('noise'), curvature: strength('curvature'),
      reflection: strength('reflection'), afterglow: strength('afterglow'),
      sweep: typeof value.sweep === 'boolean' ? value.sweep : DEFAULT_APPEARANCE.sweep,
    }
  } catch {
    return { ...DEFAULT_APPEARANCE }
  }
}

export function saveTerminalAppearance(appearance: TerminalAppearance): void {
  try { localStorage.setItem(STORAGE_KEY, JSON.stringify(appearance)) } catch { /* Storage may be unavailable. */ }
}

export function terminalAppearanceOptions(appearance: TerminalAppearance): Partial<ITerminalOptions> {
  const profile = TERMINAL_PROFILES[appearance.profile]
  // Keep ANSI colors distinct for ls, compiler diagnostics, vim, and top.
  const theme: ITheme = {
    background: profile.background, foreground: profile.foreground,
    cursor: profile.foreground, cursorAccent: profile.background,
    selectionBackground: `rgba(${profile.rgb}, 0.28)`,
    selectionInactiveBackground: `rgba(${profile.rgb}, 0.15)`,
    black: '#1a1d29', red: '#f87171', green: '#4ade80', yellow: '#fbbf24',
    blue: '#60a5fa', magenta: '#c084fc', cyan: '#22d3ee', white: '#e6e9ef',
    brightBlack: '#89919e', brightRed: '#fca5a5', brightGreen: '#86efac',
    brightYellow: '#fde68a', brightBlue: '#93c5fd', brightMagenta: '#d8b4fe',
    brightCyan: '#67e8f9', brightWhite: '#f9fafb',
  }
  return {
    theme,
    cursorStyle: appearance.enabled ? 'block' : 'bar',
    cursorBlink: !window.matchMedia('(prefers-reduced-motion: reduce)').matches,
  }
}
