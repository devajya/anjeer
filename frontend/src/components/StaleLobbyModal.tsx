import { useEffect, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import './StaleLobbyModal.css'

interface Props {
  title?:   string
  message?: string
}

const REDIRECT_DELAY_S = 3

// AGENT-CTX: Reused for two "lobby gone" scenarios:
//   1. LobbyRoom: lobby 404 or status=finished/closed  ("This game has ended")
//   2. Game: queue_overflow on direct URL access        ("This lobby is full")
// Both cases auto-redirect to /lobby after REDIRECT_DELAY_S seconds.
export function StaleLobbyModal({
  title   = 'This game has ended',
  message = '',
}: Props) {
  const navigate   = useNavigate()
  const [countdown, setCountdown] = useState(REDIRECT_DELAY_S)

  useEffect(() => {
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
  }, [navigate])

  return (
    <div className="stale-modal" role="dialog" aria-modal="true" aria-label={title}>
      <div className="stale-modal__card">
        <h2 className="stale-modal__title">{title}</h2>
        {message && <p className="stale-modal__message">{message}</p>}
        <p className="stale-modal__redirect">
          Returning to lobbies in <strong>{countdown}s</strong>…
        </p>
        <button className="stale-modal__btn" onClick={() => navigate('/lobby')}>
          Return now
        </button>
      </div>
    </div>
  )
}
