import { render, screen } from '@testing-library/react'
import { describe, it, expect, vi, beforeEach } from 'vitest'
import { ExpandingPortal } from '../ExpandingPortal'

vi.mock('gsap', () => ({
  gsap: { registerPlugin: vi.fn(), to: vi.fn() },
}))

vi.mock('gsap/ScrollTrigger', () => ({
  ScrollTrigger: {
    create: vi.fn(() => ({ kill: vi.fn() })),
    getAll: vi.fn(() => []),
  },
}))

// Strip framer-motion layout/animation props so jsdom doesn't warn on unknown attrs
vi.mock('framer-motion', async () => {
  const { forwardRef } = await import('react')
  return {
    motion: {
      div: forwardRef<
        HTMLDivElement,
        React.HTMLAttributes<HTMLDivElement> & {
          layout?: unknown
          transition?: unknown
          initial?: unknown
          animate?: unknown
          variants?: unknown
        }
      >(({ layout: _l, transition: _t, initial: _i, animate: _a, variants: _v, children, ...rest }, ref) => (
        <div ref={ref} {...rest}>{children}</div>
      )),
    },
  }
})

beforeEach(() => {
  vi.clearAllMocks()
})

describe('ExpandingPortal', () => {
  it('renders card in collapsed/inset state by default (not full-screen)', () => {
    render(<ExpandingPortal reducedMotion={false} isMobile={false} />)
    const card = screen.getByTestId('ep-card')
    expect(card.getAttribute('data-expanded')).toBe('false')
    expect(card.classList.contains('ep-card--collapsed')).toBe(true)
    expect(card.classList.contains('ep-card--expanded')).toBe(false)
  })

  it('renders in expanded full-screen state when reducedMotion=true', () => {
    render(<ExpandingPortal reducedMotion={true} isMobile={false} />)
    const card = screen.getByTestId('ep-card')
    expect(card.getAttribute('data-expanded')).toBe('true')
    expect(card.classList.contains('ep-card--expanded')).toBe(true)
    expect(card.classList.contains('ep-card--collapsed')).toBe(false)
  })

  it('renders in expanded state when isMobile=true (no card intro)', () => {
    render(<ExpandingPortal reducedMotion={false} isMobile={true} />)
    const card = screen.getByTestId('ep-card')
    expect(card.getAttribute('data-expanded')).toBe('true')
    expect(card.classList.contains('ep-card--expanded')).toBe(true)
  })

  it('parallax layer divs have different data-intensity attributes', () => {
    render(<ExpandingPortal reducedMotion={true} isMobile={false} />)
    const layers = document.querySelectorAll('[data-intensity]')
    const intensities = Array.from(layers).map((l) => l.getAttribute('data-intensity'))
    expect(intensities.length).toBeGreaterThanOrEqual(2)
    // Each layer has a distinct intensity value
    const unique = new Set(intensities)
    expect(unique.size).toBeGreaterThanOrEqual(2)
    expect(intensities).toContain('18')
    expect(intensities).toContain('32')
  })
})
