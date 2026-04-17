import { render, screen, fireEvent } from '@testing-library/react'
import { describe, test, expect, vi } from 'vitest'
import { RoundEndModal } from './RoundEndModal'
import type { RoundEndMessage } from '../types/messages'

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

const ROUND_END: RoundEndMessage = {
  type: 'round_end',
  goal_suit: 'hearts',
  results: [
    { player_slot: 0, goal_cards_held: 3, payout: 60,  balance: 110, disconnected: false },
    { player_slot: 1, goal_cards_held: 1, payout: 20,  balance: 70,  disconnected: false },
    { player_slot: 2, goal_cards_held: 0, payout: 0,   balance: 50,  disconnected: true  },
  ],
}

function renderModal(overrides: Partial<Parameters<typeof RoundEndModal>[0]> = {}) {
  const onDismiss = vi.fn()
  render(
    <RoundEndModal
      roundEnd={ROUND_END}
      playerSlot={0}
      onDismiss={onDismiss}
      {...overrides}
    />,
  )
  return { onDismiss }
}

// ---------------------------------------------------------------------------
// Goal suit reveal
// ---------------------------------------------------------------------------

describe('RoundEndModal — goal suit reveal', () => {
  test('renders goal suit name', () => {
    renderModal()
    // "Hearts was the goal suit" — match case-insensitively
    expect(screen.getByText(/hearts was the goal suit/i)).toBeTruthy()
  })

  test('renders goal suit Unicode icon', () => {
    renderModal()
    expect(screen.getByText('♥')).toBeTruthy()
  })

  test('renders icon for clubs', () => {
    renderModal({
      roundEnd: { ...ROUND_END, goal_suit: 'clubs' },
    })
    expect(screen.getByText('♣')).toBeTruthy()
  })

  test('renders icon for diamonds', () => {
    renderModal({
      roundEnd: { ...ROUND_END, goal_suit: 'diamonds' },
    })
    expect(screen.getByText('♦')).toBeTruthy()
  })

  test('renders icon for spades', () => {
    renderModal({
      roundEnd: { ...ROUND_END, goal_suit: 'spades' },
    })
    expect(screen.getByText('♠')).toBeTruthy()
  })
})

// ---------------------------------------------------------------------------
// Per-player standings table
// ---------------------------------------------------------------------------

describe('RoundEndModal — standings table', () => {
  test('renders a row for every player in results', () => {
    renderModal()
    expect(screen.getByText('Player 0')).toBeTruthy()
    expect(screen.getByText('Player 1')).toBeTruthy()
    expect(screen.getByText('Player 2')).toBeTruthy()
  })

  test('marks disconnected player with (away)', () => {
    renderModal()
    // Player 2 is disconnected
    expect(screen.getByText('(away)')).toBeTruthy()
  })

  test('does not mark connected players with (away)', () => {
    renderModal()
    const awayNodes = screen.queryAllByText('(away)')
    // Only player 2 is disconnected → exactly one (away)
    expect(awayNodes).toHaveLength(1)
  })
})

// ---------------------------------------------------------------------------
// Own row highlighting
// ---------------------------------------------------------------------------

describe('RoundEndModal — own row highlighting', () => {
  test('own row carries --own modifier class', () => {
    renderModal({ playerSlot: 1 })
    const rows = document.querySelectorAll('.round-end-modal__row')
    const ownRow = Array.from(rows).find(r =>
      r.classList.contains('round-end-modal__row--own'),
    )
    expect(ownRow).toBeTruthy()
    expect(ownRow?.textContent).toContain('Player 1')
  })

  test('exactly one row is marked as own', () => {
    renderModal({ playerSlot: 0 })
    const ownRows = document.querySelectorAll('.round-end-modal__row--own')
    expect(ownRows.length).toBe(1)
  })

  test('no row is highlighted when playerSlot is null', () => {
    renderModal({ playerSlot: null })
    const ownRows = document.querySelectorAll('.round-end-modal__row--own')
    expect(ownRows.length).toBe(0)
  })
})

// ---------------------------------------------------------------------------
// Own summary panel
// ---------------------------------------------------------------------------

describe('RoundEndModal — own summary', () => {
  test('shows own payout and balance', () => {
    renderModal({ playerSlot: 0 })
    // Player 0: payout=60, balance=110
    const summary = document.querySelector('.round-end-modal__own-summary')
    expect(summary).toBeTruthy()
    expect(summary?.textContent).toContain('60')
    expect(summary?.textContent).toContain('110')
  })

  test('own summary is absent when playerSlot is null', () => {
    renderModal({ playerSlot: null })
    expect(document.querySelector('.round-end-modal__own-summary')).toBeNull()
  })
})

// ---------------------------------------------------------------------------
// Dismiss behaviour
// ---------------------------------------------------------------------------

describe('RoundEndModal — dismiss', () => {
  test('calls onDismiss when Dismiss button is clicked', () => {
    const { onDismiss } = renderModal()
    fireEvent.click(screen.getByRole('button', { name: /dismiss/i }))
    expect(onDismiss).toHaveBeenCalledTimes(1)
  })

  test('calls onDismiss when backdrop is clicked', () => {
    const { onDismiss } = renderModal()
    const backdrop = document.querySelector('.round-end-backdrop')!
    fireEvent.click(backdrop)
    expect(onDismiss).toHaveBeenCalledTimes(1)
  })

  test('does not call onDismiss when modal card is clicked', () => {
    const { onDismiss } = renderModal()
    const card = document.querySelector('.round-end-modal')!
    fireEvent.click(card)
    expect(onDismiss).not.toHaveBeenCalled()
  })
})
