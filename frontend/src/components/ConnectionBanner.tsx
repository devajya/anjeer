import './ConnectionBanner.css'

interface Props {
  connected: boolean
}

// data-testid attributes are stable test hooks — do not remove when refactoring markup.
export function ConnectionBanner({ connected }: Props) {
  const statusText = connected ? 'Connected' : 'Disconnected'

  return (
    <div className={`connection-banner ${connected ? 'connected' : 'disconnected'}`}>
      <span data-testid="status" className="connection-banner__status">
        {statusText}
      </span>
    </div>
  )
}
