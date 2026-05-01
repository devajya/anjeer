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
  vote_count: 1,
  votes_required: 2,
  next_round_at: new Date(Date.now() + 30_000).toISOString(),
}

const defaultProps = {
  interRound: INTER_ROUND_MSG,
  liveVotes: 1,
  liveVotesRequired: 2,
  playerSlot: 0,
  hasVoted: false,
  onVoteToEnd: vi.fn(),
  onCountdownExpired: vi.fn(),
}

beforeEach(() => {
  vi.useFakeTimers()
})

afterEach(() => {
  vi.useRealTimers()
  vi.clearAllMocks()
})

// ─── F1: Inter-round screen renders vote tally ────────────────────────────────

describe('InterRoundScreen renders vote tally', () => {
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

  test('renders live vote tally', () => {
    render(<InterRoundScreen {...defaultProps} liveVotes={1} liveVotesRequired={2} />)

    expect(screen.getByText('Votes to end game:')).toBeInTheDocument()
    // AGENT-CTX: VoteTally renders votes in <strong> and required as a text node
    // within the same span, so getByText('2') won't match the split content.
    // Query the container directly and check its combined text.
    const count = document.querySelector('.vote-tally__count')
    expect(count?.textContent?.replace(/\s+/g, ' ').trim()).toBe('1 / 2')
  })

  test('vote button is enabled when hasVoted is false', () => {
    render(<InterRoundScreen {...defaultProps} hasVoted={false} />)

    const btn = screen.getByRole('button', { name: /Vote to End Game/i })
    expect(btn).not.toBeDisabled()
  })

  test('vote button is disabled and shows "Vote Cast" after voting', () => {
    render(<InterRoundScreen {...defaultProps} hasVoted={true} />)

    const btn = screen.getByRole('button', { name: /Vote Cast/i })
    expect(btn).toBeDisabled()
  })

  test('clicking vote button calls onVoteToEnd', () => {
    const onVoteToEnd = vi.fn()
    render(<InterRoundScreen {...defaultProps} onVoteToEnd={onVoteToEnd} />)

    fireEvent.click(screen.getByRole('button', { name: /Vote to End Game/i }))
    expect(onVoteToEnd).toHaveBeenCalledOnce()
  })

  test('countdown renders seconds remaining', () => {
    render(<InterRoundScreen {...defaultProps} />)

    // After initial render the timer effect runs; fake timers start from now.
    // The next_round_at is 30s in the future so countdown shows ~30.
    act(() => { vi.advanceTimersByTime(500) })

    expect(screen.getByText(/Next round in/i)).toBeInTheDocument()
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

  test('hides vote button when next_round_at is null (vote threshold met)', () => {
    const props = {
      ...defaultProps,
      interRound: { ...INTER_ROUND_MSG, next_round_at: null },
    }
    render(<InterRoundScreen {...props} />)

    expect(screen.queryByRole('button', { name: /Vote/i })).not.toBeInTheDocument()
    expect(screen.getByText(/Vote threshold reached/i)).toBeInTheDocument()
  })
})
