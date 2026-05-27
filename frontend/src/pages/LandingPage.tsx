import { lazy, Suspense } from 'react'
import { useReducedMotion } from 'framer-motion'
import { useMediaQuery } from '../hooks/useMediaQuery'
import { ExpandingPortal } from '../components/ExpandingPortal'

// Below-fold sections loaded on demand — none are visible on initial render.
// ExpandingPortal stays static: it is the above-fold hero section.
const AutoAdvanceProgress  = lazy(() => import('../components/AutoAdvanceProgress').then(m => ({ default: m.AutoAdvanceProgress })))
const ScrollTrackerSection = lazy(() => import('../components/ScrollTrackerSection').then(m => ({ default: m.ScrollTrackerSection })))
const MathDive             = lazy(() => import('../components/MathDive').then(m => ({ default: m.MathDive })))

// AGENT-CTX: Post-OAuth the server now redirects to /lobby directly, so
// LandingPage no longer needs to bounce authenticated users. Authenticated
// users can visit / freely (e.g. via the AppNav wordmark).
export function LandingPage() {
  const prefersReducedMotion = useReducedMotion() ?? false
  const isMobile = useMediaQuery('(max-width: 768px)')

  return (
    <main style={{ background: 'var(--color-bg)', minHeight: '100vh' }}>
      <ExpandingPortal reducedMotion={prefersReducedMotion} isMobile={isMobile} />
      <Suspense fallback={null}>
        <AutoAdvanceProgress reducedMotion={prefersReducedMotion} />
        <ScrollTrackerSection reducedMotion={prefersReducedMotion} />
        <MathDive reducedMotion={prefersReducedMotion} />
      </Suspense>
    </main>
  )
}
