import { useEffect, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import type { ReconnectStatus } from '../hooks/useReconnect'
import './ReconnectOverlay.css'

interface Props {
  status: ReconnectStatus
  /** lobbyId of the active session — used to auto-queue on redirect. */
  lobbyId: string
}

const REDIRECT_DELAY_S = 4

// AGENT-CTX: Only shown on 'expired' — the slot window closed while the player
// was disconnected. During active reconnect (status='reconnecting') the game
// board stays visible so the player can trade the moment reattach succeeds.
// Auto-redirects to /lobby?queue_for=<lobbyId> so LobbyBrowser auto-enqueues them.
export function ReconnectOverlay({ status, lobbyId }: Props) {
  const navigate = useNavigate()
  const [countdown, setCountdown] = useState(REDIRECT_DELAY_S)

  useEffect(() => {
    if (status !== 'expired') return
    setCountdown(REDIRECT_DELAY_S)
    const id = setInterval(() => {
      setCountdown(s => {
        if (s <= 1) {
          clearInterval(id)
          navigate(lobbyId ? `/lobby?queue_for=${lobbyId}` : '/lobby')
          return 0
        }
        return s - 1
      })
    }, 1000)
    return () => clearInterval(id)
  }, [status, navigate, lobbyId])

  if (status !== 'expired') return null

  return (
    <div className="reconnect-overlay" role="alert" aria-live="assertive">
      <div className="reconnect-overlay__card">
        <h2 className="reconnect-overlay__title">Slot expired</h2>
        <p className="reconnect-overlay__hint">
          Your reconnect window closed while you were away. You will be placed in the queue for the next available slot.
        </p>
        <p className="reconnect-overlay__redirect">
          Joining queue in <strong>{countdown}s</strong>…
        </p>
        <button
          className="reconnect-overlay__btn"
          onClick={() => navigate(lobbyId ? `/lobby?queue_for=${lobbyId}` : '/lobby')}
        >
          Join queue now
        </button>
      </div>
    </div>
  )
}
