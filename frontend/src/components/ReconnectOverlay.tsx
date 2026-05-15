import { useEffect, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import type { ReconnectStatus } from '../hooks/useReconnect'
import './ReconnectOverlay.css'

interface Props {
  status: ReconnectStatus
}

const REDIRECT_DELAY_S = 4

// AGENT-CTX: Only shown on 'expired' — the slot window closed while the player
// was disconnected. During active reconnect (status='reconnecting') the game
// board stays visible so the player can trade the moment reattach succeeds.
// Auto-redirects to /lobby after REDIRECT_DELAY_S seconds.
export function ReconnectOverlay({ status }: Props) {
  const navigate = useNavigate()
  const [countdown, setCountdown] = useState(REDIRECT_DELAY_S)

  useEffect(() => {
    if (status !== 'expired') return
    setCountdown(REDIRECT_DELAY_S)
    const id = setInterval(() => {
      setCountdown(s => {
        if (s <= 1) {
          clearInterval(id)
          navigate('/lobby')
          return 0
        }
        return s - 1
      })
    }, 1000)
    return () => clearInterval(id)
  }, [status, navigate])

  if (status !== 'expired') return null

  return (
    <div className="reconnect-overlay" role="alert" aria-live="assertive">
      <div className="reconnect-overlay__card">
        <h2 className="reconnect-overlay__title">Slot expired</h2>
        <p className="reconnect-overlay__hint">
          Your reconnect window closed while you were away.
        </p>
        <p className="reconnect-overlay__redirect">
          Returning to lobby in <strong>{countdown}s</strong>…
        </p>
      </div>
    </div>
  )
}
