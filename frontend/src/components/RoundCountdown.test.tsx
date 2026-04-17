import { render, screen, act } from '@testing-library/react'
import { describe, test, expect, beforeEach, afterEach, vi } from 'vitest'
import { RoundCountdown } from './RoundCountdown'

// ---------------------------------------------------------------------------
// AC: Active-round countdown — MM:SS format, expiring class, priority logic
// ---------------------------------------------------------------------------

function futureISO(offsetMs: number): string {
  return new Date(Date.now() + offsetMs).toISOString()
}

describe('RoundCountdown — null renders nothing', () => {
  test('renders nothing when both props are null', () => {
    const { container } = render(<RoundCountdown startsAt={null} roundEndAt={null} />)
    expect(container.firstChild).toBeNull()
  })
})

describe('RoundCountdown — pre-deal path', () => {
  beforeEach(() => { vi.useFakeTimers() })
  afterEach(() => { vi.useRealTimers() })

  test('shows seconds-only countdown when only startsAt is set', () => {
    const startsAt = new Date(Date.now() + 10_000).toISOString()
    render(<RoundCountdown startsAt={startsAt} roundEndAt={null} />)
    expect(screen.getByRole('status')).toHaveTextContent('Round starting in')
    expect(screen.getByRole('status')).toHaveTextContent('10')
  })

  test('hides pre-deal display once elapsed (seconds ≤ 0)', () => {
    const startsAt = new Date(Date.now() - 1_000).toISOString()
    render(<RoundCountdown startsAt={startsAt} roundEndAt={null} />)
    // secondsLeft will be 0, so pre-deal path returns null
    expect(screen.queryByRole('status')).toBeNull()
  })
})

describe('RoundCountdown — active-round path', () => {
  beforeEach(() => { vi.useFakeTimers() })
  afterEach(() => { vi.useRealTimers() })

  test('renders MM:SS format for active round', () => {
    // 125 seconds → 02:05
    const roundEndAt = new Date(Date.now() + 125_000).toISOString()
    render(<RoundCountdown startsAt={null} roundEndAt={roundEndAt} />)
    expect(screen.getByRole('timer')).toHaveTextContent('02:05')
  })

  test('renders 00:00 when round has already expired', () => {
    const roundEndAt = new Date(Date.now() - 5_000).toISOString()
    render(<RoundCountdown startsAt={null} roundEndAt={roundEndAt} />)
    expect(screen.getByRole('timer')).toHaveTextContent('00:00')
  })

  test('does not add --expiring class when seconds > 30', () => {
    const roundEndAt = new Date(Date.now() + 60_000).toISOString()
    render(<RoundCountdown startsAt={null} roundEndAt={roundEndAt} />)
    const el = screen.getByRole('timer')
    expect(el.className).not.toContain('round-countdown--expiring')
    expect(el.className).toContain('round-countdown--active')
  })

  test('adds --expiring class when seconds ≤ 30', () => {
    const roundEndAt = new Date(Date.now() + 20_000).toISOString()
    render(<RoundCountdown startsAt={null} roundEndAt={roundEndAt} />)
    expect(screen.getByRole('timer').className).toContain('round-countdown--expiring')
  })

  test('ticks down over time', () => {
    const roundEndAt = new Date(Date.now() + 62_000).toISOString()
    render(<RoundCountdown startsAt={null} roundEndAt={roundEndAt} />)
    expect(screen.getByRole('timer')).toHaveTextContent('01:02')
    act(() => { vi.advanceTimersByTime(3_000) })
    expect(screen.getByRole('timer')).toHaveTextContent('00:59')
  })
})

describe('RoundCountdown — roundEndAt takes priority over startsAt', () => {
  test('shows active-round timer (MM:SS) when both props are set', () => {
    const startsAt  = new Date(Date.now() + 5_000).toISOString()
    const roundEndAt = new Date(Date.now() + 90_000).toISOString()
    render(<RoundCountdown startsAt={startsAt} roundEndAt={roundEndAt} />)
    // Should render role="timer" (active), not role="status" (pre-deal)
    expect(screen.getByRole('timer')).toBeInTheDocument()
    expect(screen.queryByRole('status')).toBeNull()
    expect(screen.getByRole('timer')).toHaveTextContent('01:30')
  })
})
