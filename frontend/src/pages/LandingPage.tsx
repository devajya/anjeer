import { Navigate } from 'react-router-dom'
import { useReducedMotion } from 'framer-motion'
import { useAuth } from '../hooks/useAuth'
import { useMediaQuery } from '../hooks/useMediaQuery'
import { ExpandingPortal } from '../components/ExpandingPortal'
import { ScrollTrackerSection } from '../components/ScrollTrackerSection'

// AGENT-CTX: OAuth callback redirects to cors_origin (/) after setting cookies.
// Authenticated users are bounced to /lobby here so the post-auth flow still
// lands in the lobby browser. Mirrors the same guard in Login.tsx (authed →
// /lobby) but in the opposite direction (unauthenticated → show landing page).
export function LandingPage() {
  const { user, loading } = useAuth()
  const prefersReducedMotion = useReducedMotion() ?? false
  const isMobile = useMediaQuery('(max-width: 768px)')

  if (loading) return null
  if (user !== null) return <Navigate to="/lobby" replace />

  return (
    <main style={{ background: 'var(--color-bg)', minHeight: '100vh' }}>
      <ExpandingPortal reducedMotion={prefersReducedMotion} isMobile={isMobile} />
      <ScrollTrackerSection reducedMotion={prefersReducedMotion} />
    </main>
  )
}
