import './ConnectionBanner.css'

interface Props {
  connected: boolean
  /** Last heartbeat server_ts (unix ms), or null if no heartbeat received yet. */
  lastServerTs: number | null
}

// AGENT-CTX: data-testid attributes are the stable test hooks — do not remove them
// even when refactoring the markup. Tests select by testid, not by class or text.
export function ConnectionBanner({ connected, lastServerTs }: Props) {
  const statusText = connected ? 'Connected' : 'Disconnected'

  // AGENT-CTX: toLocaleString() is intentional — human-readable local time is more
  // useful here than an ISO string. If machine-parseable output is ever needed,
  // add a separate data attribute rather than changing this display format.
  const timestampText = lastServerTs !== null
    ? new Date(lastServerTs).toLocaleString()
    : '—'

  return (
    <div className={`connection-banner ${connected ? 'connected' : 'disconnected'}`}>
      <span data-testid="status" className="connection-banner__status">
        {statusText}
      </span>
      <span className="connection-banner__label">Last heartbeat:</span>
      <span data-testid="timestamp" className="connection-banner__timestamp">
        {timestampText}
      </span>
    </div>
  )
}
