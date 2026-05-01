import { useNavigate } from 'react-router-dom'
import type { SessionErrorMessage } from '../types/messages'
import './SessionError.css'

interface SessionErrorProps {
  sessionError: SessionErrorMessage
}

export function SessionError({ sessionError }: SessionErrorProps) {
  const navigate = useNavigate()

  return (
    // AGENT-CTX: role="alert" (not "dialog") — the error arrived asynchronously
    // and must be announced immediately by screen readers without user focus.
    // aria-live is implicit on role="alert" (assertive).
    <div
      className="se__backdrop"
      role="alert"
      aria-label="Session error"
    >
      <div className="se__card">
        <h2 className="se__title">Session Error</h2>
        <p className="se__message">{sessionError.message}</p>
        {/* AGENT-CTX: TODO(observability) — when session_error gains error_id/
            session_id fields (Slice observability pass), display them here in a
            <details> block so players can report the ID to support. */}
        <button className="se__return-btn" onClick={() => navigate('/lobby')}>
          Return to Lobby
        </button>
      </div>
    </div>
  )
}
