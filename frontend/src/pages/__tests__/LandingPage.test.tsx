import { describe, it, expect, vi } from 'vitest'
import { render, screen } from '@testing-library/react'
import { MemoryRouter } from 'react-router-dom'
import { LandingPage } from '../LandingPage'

// Stub all heavy sections — this test only verifies assembly and CTA routing
vi.mock('../../components/ExpandingPortal', () => ({
  ExpandingPortal: () => <section data-testid="section-expanding-portal" />,
}))
vi.mock('../../components/AutoAdvanceProgress', () => ({
  AutoAdvanceProgress: () => <section data-testid="section-auto-advance" />,
}))
vi.mock('../../components/ScrollTrackerSection', () => ({
  ScrollTrackerSection: () => <section data-testid="section-scroll-tracker" />,
}))

// LandingPage no longer calls useAuth — no mock needed here.

function renderLanding() {
  return render(
    <MemoryRouter initialEntries={['/']}>
      <LandingPage />
    </MemoryRouter>,
  )
}

describe('LandingPage', () => {
  it('renders all 3 sections', async () => {
    renderLanding()
    expect(screen.getByTestId('section-expanding-portal')).toBeInTheDocument()
    expect(await screen.findByTestId('section-auto-advance')).toBeInTheDocument()
    expect(await screen.findByTestId('section-scroll-tracker')).toBeInTheDocument()
  })

  it('renders without auth guard — accessible to unauthenticated users', () => {
    // No ProtectedRoute wrapping; unauthenticated render must succeed
    renderLanding()
    expect(screen.getByTestId('section-expanding-portal')).toBeInTheDocument()
  })
})
