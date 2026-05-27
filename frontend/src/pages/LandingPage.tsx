import { useReducedMotion } from 'framer-motion'
import { useMediaQuery } from '../hooks/useMediaQuery'
import { ExpandingPortal } from '../components/ExpandingPortal'
import { AutoAdvanceProgress } from '../components/AutoAdvanceProgress'
import { ScrollTrackerSection } from '../components/ScrollTrackerSection'
import { MathDive } from '../components/MathDive'

// AGENT-CTX: Post-OAuth the server now redirects to /lobby directly, so
// LandingPage no longer needs to bounce authenticated users. Authenticated
// users can visit / freely (e.g. via the AppNav wordmark).
export function LandingPage() {
  const prefersReducedMotion = useReducedMotion() ?? false
  const isMobile = useMediaQuery('(max-width: 768px)')

  return (
    <main style={{ background: 'var(--color-bg)', minHeight: '100vh' }}>
      <ExpandingPortal reducedMotion={prefersReducedMotion} isMobile={isMobile} />
      <AutoAdvanceProgress reducedMotion={prefersReducedMotion} />
      <ScrollTrackerSection reducedMotion={prefersReducedMotion} />
      <MathDive reducedMotion={prefersReducedMotion} />
    </main>
  )
}
