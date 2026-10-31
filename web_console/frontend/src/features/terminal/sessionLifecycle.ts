// Keep logout cleanup synchronous without loading xterm on non-terminal pages.
let cleanup: (() => void) | undefined

export function registerTerminalCleanup(callback: () => void): void {
  cleanup = callback
}

export function destroyAllTerminals(): void {
  cleanup?.()
}
