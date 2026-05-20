import { render, screen, fireEvent, act } from '@testing-library/react'
import { describe, test, expect, vi, beforeEach, afterEach } from 'vitest'
import { InterRoundScreen } from '../InterRoundScreen'
import type { InterRoundMessage } from '../../types/messages'

// AGENT-CTX: vi.useFakeTimers lets tests control the countdown without real waits.
// afterEach restores real timers so other test files are not affected.

const INTER_ROUND_MSG: InterRoundMessage = {
  type: 'inter_round',
  round_number: 1,
  goal_suit: 'hearts',
  results: [
    { player_slot: 0, goal_cards_held: 3, payout: 80, balance: 160, disconnected: false },
    { player_slot: 1, goal_cards_held: 1, payout: 20, balance: 100, disconnected: false },
  ],
  next_round_at: new Date(Date.now() + 30_000).toISOString(),
}

const defaultProps = {
  interRound: INTER_ROUND_MSG,
  playerSlot: 0,
  isOwner: false,
  ownerUsername: 'alice',
  onStartNextRound: vi.fn(),
  onEndGame: vi.fn(),
  onCountdownExpired: vi.fn(),
}

beforeEach(() => {
  vi.useFakeTimers()
})

afterEach(() => {
  vi.useRealTimers()
  vi.clearAllMocks()
})

// ─── F1: Inter-round screen renders standings and owner controls ──────────────

describe('InterRoundScreen', () => {
  test('renders round number and goal suit', () => {
    render(<InterRoundScreen {...defaultProps} />)

    expect(screen.getByText(/Round 1 Finished/i)).toBeInTheDocument()
    expect(screen.getByText(/Hearts was the goal suit/i)).toBeInTheDocument()
  })

  test('renders standings table rows for each result', () => {
    render(<InterRoundScreen {...defaultProps} />)

    expect(screen.getByText('Player 0')).toBeInTheDocument()
    expect(screen.getByText('Player 1')).toBeInTheDocument()
  })

  test('non-owner sees waiting message', () => {
    render(<InterRoundScreen {...defaultProps} isOwner={false} ownerUsername="alice" />)

    expect(screen.getByText(/Waiting for/i)).toBeInTheDocument()
    expect(screen.getByText(/alice/i)).toBeInTheDocument()
    expect(screen.queryByRole('button', { name: /Start Next Round/i })).not.toBeInTheDocument()
  })

  test('owner sees Start Next Round button', () => {
    render(<InterRoundScreen {...defaultProps} isOwner={true} />)

    expect(screen.getByRole('button', { name: /Start Next Round/i })).toBeInTheDocument()
  })

  test('owner sees End Game button', () => {
    render(<InterRoundScreen {...defaultProps} isOwner={true} />)

    expect(screen.getByRole('button', { name: /End Game/i })).toBeInTheDocument()
  })

  test('clicking Start Next Round calls onStartNextRound', () => {
    const onStartNextRound = vi.fn()
    render(<InterRoundScreen {...defaultProps} isOwner={true} onStartNextRound={onStartNextRound} />)

    fireEvent.click(screen.getByRole('button', { name: /Start Next Round/i }))
    expect(onStartNextRound).toHaveBeenCalledOnce()
  })

  test('countdown renders seconds remaining', () => {
    render(<InterRoundScreen {...defaultProps} />)

    act(() => { vi.advanceTimersByTime(500) })

    expect(screen.getByText(/Auto-start in/i)).toBeInTheDocument()
  })

  test('calls onCountdownExpired when timer reaches zero', () => {
    const onCountdownExpired = vi.fn()
    render(<InterRoundScreen {...defaultProps} onCountdownExpired={onCountdownExpired} />)

    act(() => { vi.advanceTimersByTime(31_000) })

    expect(onCountdownExpired).toHaveBeenCalledOnce()
  })

  test('own player row is highlighted', () => {
    render(<InterRoundScreen {...defaultProps} playerSlot={0} />)

    const rows = document.querySelectorAll('tr.irs__row--own')
    expect(rows).toHaveLength(1)
  })

  test('spectator sees neither owner controls nor waiting message', () => {
    render(<InterRoundScreen {...defaultProps} isSpectator />)

    expect(screen.queryByRole('button', { name: /Start Next Round/i })).not.toBeInTheDocument()
    expect(screen.queryByText(/Waiting for/i)).not.toBeInTheDocument()
  })
})
