import { Navigate } from 'react-router-dom'
import { useAuth } from '../hooks/useAuth'
import './Login.css'

// AGENT-CTX: OAuth redirects use relative paths (/auth/{provider}) so that
// the Vite dev proxy (→ :8080) and the production nginx proxy (/auth → :8080)
// both work without any environment-specific URL. Do NOT hardcode localhost:8080.
const PROVIDERS = [
  { id: 'github', label: 'Continue with GitHub' },
  { id: 'google', label: 'Continue with Google' },
] as const

export function Login() {
  const { user, loading } = useAuth()

  // Block forward navigation to /login when the user is already authenticated.
  if (loading) return null
  if (user !== null) return <Navigate to="/lobby" replace />

  return (
    <div className="login">
      <h1 className="login__title">Anjeer</h1>
      <p className="login__subtitle">Sign in to play</p>
      <div className="login__providers">
        {PROVIDERS.map(p => (
          <button
            key={p.id}
            className="login__btn"
            onClick={() => { window.location.href = `/auth/${p.id}` }}
          >
            {p.label}
          </button>
        ))}
      </div>
    </div>
  )
}
