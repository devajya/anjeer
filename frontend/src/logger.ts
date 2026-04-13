// AGENT-CTX: Single logger instance for the entire frontend.
// - Writes every entry to the browser console immediately.
// - Batches entries and POSTs them to the server's /api/log endpoint every 500ms
//   so they appear in logs/frontend_logs.txt alongside server and engine logs.
// - Completely fire-and-forget — if the flush fails (server down, etc.) logs are
//   still visible in the browser console.
// Import `logger` directly wherever you need it — no React context required.

type Level = 'DEBUG' | 'INFO' | 'WARN' | 'ERROR'

interface LogEntry {
  ts: number        // Date.now() at time of log
  level: Level
  component: string
  message: string
  data?: unknown
}

const FLUSH_INTERVAL_MS = 500

class FrontendLogger {
  private buffer: LogEntry[] = []
  private flushTimer: ReturnType<typeof setTimeout> | null = null

  log(level: Level, component: string, message: string, data?: unknown) {
    const entry: LogEntry = { ts: Date.now(), level, component, message, data }

    // Console output: always immediate
    const dataStr = data !== undefined ? data : ''
    const tag = `[${level}][${component}]`
    switch (level) {
      case 'DEBUG': console.debug(tag, message, dataStr); break
      case 'INFO':  console.info(tag, message, dataStr);  break
      case 'WARN':  console.warn(tag, message, dataStr);  break
      case 'ERROR': console.error(tag, message, dataStr); break
    }

    this.buffer.push(entry)
    this.scheduleFlush()
  }

  debug(component: string, message: string, data?: unknown) {
    this.log('DEBUG', component, message, data)
  }
  info(component: string, message: string, data?: unknown) {
    this.log('INFO', component, message, data)
  }
  warn(component: string, message: string, data?: unknown) {
    this.log('WARN', component, message, data)
  }
  error(component: string, message: string, data?: unknown) {
    this.log('ERROR', component, message, data)
  }

  private scheduleFlush() {
    if (this.flushTimer !== null) return
    this.flushTimer = setTimeout(() => this.flush(), FLUSH_INTERVAL_MS)
  }

  private flush() {
    this.flushTimer = null
    if (this.buffer.length === 0) return

    const entries = this.buffer.splice(0)
    fetch('/api/log', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(entries),
    }).catch(() => {
      // Server unreachable — console logs are still intact, silently drop.
    })
  }
}

export const logger = new FrontendLogger()
