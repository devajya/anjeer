import { useEffect, useRef } from 'react'
import type { ScriptLogMessage } from '../types/messages'

interface Props {
  logs: ScriptLogMessage[]
}

export function ScriptLogPanel({ logs }: Props) {
  const bodyRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    const el = bodyRef.current
    if (el) el.scrollTop = el.scrollHeight
  }, [logs.length])

  return (
    <div className="script-log-panel">
      <div className="script-log-panel__header">Script Log</div>
      <div className="script-log-panel__body" ref={bodyRef}>
        {logs.length === 0 ? (
          <span className="script-log-panel__empty">No messages yet…</span>
        ) : (
          logs.map((entry, i) => (
            <div key={i} className="script-log-panel__entry">
              <span className="script-log-panel__ts">
                {new Date(entry.timestamp).toLocaleTimeString()}
              </span>
              <span className="script-log-panel__slot">P{entry.player_slot}</span>
              <span className="script-log-panel__msg">{entry.message}</span>
            </div>
          ))
        )}
      </div>
    </div>
  )
}
