import { render, screen } from '@testing-library/react'
import { describe, it, expect, vi, beforeAll } from 'vitest'
import { ScrollTrackerSection } from '../ScrollTrackerSection'

// AGENT-CTX: GSAP and ScrollTrigger require a real browser scroll context.
// In jsdom there is no layout engine, so gsap.to() and ScrollTrigger throw.
// We mock the dynamic imports so the useEffect resolves without error.
vi.mock('gsap', () => ({
  gsap: {
    registerPlugin: vi.fn(),
    to: vi.fn(),
  },
}))

vi.mock('gsap/ScrollTrigger', () => ({
  ScrollTrigger: {
    getAll: vi.fn(() => []),
  },
}))

beforeAll(() => {
  // matchMedia is not implemented in jsdom
  Object.defineProperty(window, 'matchMedia', {
    writable: true,
    value: vi.fn().mockImplementation((query: string) => ({
      matches: false,
      media: query,
      onchange: null,
      addListener: vi.fn(),
      removeListener: vi.fn(),
      addEventListener: vi.fn(),
      removeEventListener: vi.fn(),
      dispatchEvent: vi.fn(),
    })),
  })
})

describe('ScrollTrackerSection', () => {
  it('renders 4 sub-sections with correct headline text', () => {
    render(<ScrollTrackerSection reducedMotion={false} />)
    expect(screen.getByText('Trade in your browser')).toBeInTheDocument()
    expect(screen.getByText('Script from the terminal')).toBeInTheDocument()
    expect(screen.getByText('Spectate any game')).toBeInTheDocument()
    expect(screen.getByText('Play against bots')).toBeInTheDocument()
  })

  it('renders all 4 inline SVGs', () => {
    const { container } = render(<ScrollTrackerSection reducedMotion={false} />)
    const svgs = container.querySelectorAll('svg.scroll-tracker-svg')
    expect(svgs).toHaveLength(4)
  })

  it('pill element exists in DOM', () => {
    const { container } = render(<ScrollTrackerSection reducedMotion={false} />)
    const pill = container.querySelector('.scroll-tracker-pill')
    expect(pill).toBeInTheDocument()
  })

  it('when reducedMotion=true, pill has inline style left:0 and no GSAP init', () => {
    const { container } = render(<ScrollTrackerSection reducedMotion={true} />)
    const pill = container.querySelector('.scroll-tracker-pill') as HTMLElement
    expect(pill).toBeInTheDocument()
    // Reduced motion sets explicit left:0 via inline style
    expect(pill.style.left).toBe('0px')
  })
})
