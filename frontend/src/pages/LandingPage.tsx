import { Navigate } from 'react-router-dom'
import { useAuth } from '../hooks/useAuth'

// AGENT-CTX: OAuth callback redirects to cors_origin (/) after setting cookies.
// Authenticated users are bounced to /lobby here so the post-auth flow still
// lands in the lobby browser. Mirrors the same guard in Login.tsx (authed →
// /lobby) but in the opposite direction (unauthenticated → show landing page).
export function LandingPage() {
  const { user, loading } = useAuth()
  if (loading) return null
  if (user !== null) return <Navigate to="/lobby" replace />
  return <div>Landing Page Stub</div>
}
