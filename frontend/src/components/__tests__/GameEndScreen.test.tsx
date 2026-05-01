import { render, screen, fireEvent } from '@testing-library/react'
import { describe, test, expect, vi } from 'vitest'
import { MemoryRouter } from 'react-router-dom'
import { GameEndScreen } from '../GameEndScreen'
import type { GameEndedMessage } from '../../types/messages'

const mockNavigate = vi.fn()

vi.mock('react-router-dom', async (importOriginal) => {
  const actual = await importOriginal<typeof import('react-router-dom')>()
  return { ...actual, useNavigate: () => mockNavigate }
})

// ── Fixtures ──────────────────────────────────────────────────────────────────

const GAME_ENDED_TWO_ROUNDS: GameEndedMessage = {
  type: 'game_ended',
  rounds: [
    {
      round_number: 1,
      goal_suit: 'hearts',
      results: [
        { player_slot: 0, goal_cards_held: 3, payout: 80, balance: 110, disconnected: false },
        { player_slot: 1, goal_cards_held: 1, payout: 20, balance:  90, disconnected: false },
      ],
    },
    {
      round_number: 2,
      goal_suit: 'clubs',
      results: [
        { player_slot: 0, goal_cards_held: 2, payout: 60, balance: 130, disconnected: false },
        { player_slot: 1, goal_cards_held: 3, payout: 80, balance: 110, disconnected: true },
      ],
    },
  ],
  final_standings: [
    { player_slot: 0, username: 'alice', final_balance: 130, net_change: 30 },
    { player_slot: 1, username: 'bob',   final_balance: 110, net_change: 10 },
  ],
}

function renderScreen(gameEnded = GAME_ENDED_TWO_ROUNDS, playerSlot: number | null = 0) {
  return render(
    <MemoryRouter>
      <GameEndScreen gameEnded={gameEnded} playerSlot={playerSlot} />
    </MemoryRouter>
  )
}

// ── F2: GameEndScreen shows all rounds ────────────────────────────────────────

describe('GameEndScreen shows all rounds', () => {
  test('renders "Game Over" heading', () => {
    renderScreen()
    expect(screen.getByRole('heading', { name: /Game Over/i })).toBeInTheDocument()
  })

  test('shows all round numbers', () => {
    renderScreen()
    expect(screen.getByText('Round 1')).toBeInTheDocument()
    expect(screen.getByText('Round 2')).toBeInTheDocument()
  })

  test('shows goal suit for each round', () => {
    renderScreen()
    // Hearts and Clubs appear as suit names in the round headers
    expect(screen.getByText('Hearts')).toBeInTheDocument()
    expect(screen.getByText('Clubs')).toBeInTheDocument()
  })

  test('shows final standings with usernames', () => {
    renderScreen()
    expect(screen.getByText('alice')).toBeInTheDocument()
    expect(screen.getByText('bob')).toBeInTheDocument()
  })

  test('shows positive net change with + prefix', () => {
    renderScreen()
    expect(screen.getByText('+30')).toBeInTheDocument()
    expect(screen.getByText('+10')).toBeInTheDocument()
  })

  test('shows negative net change without + prefix', () => {
    const withLoss: GameEndedMessage = {
      ...GAME_ENDED_TWO_ROUNDS,
      final_standings: [
        { player_slot: 0, username: 'alice', final_balance: 70, net_change: -30 },
      ],
    }
    renderScreen(withLoss)
    expect(screen.getByText('-30')).toBeInTheDocument()
  })

  test('marks disconnected player as away in round breakdown', () => {
    renderScreen()
    // Round 2, slot 1 is disconnected
    expect(screen.getByText('(away)')).toBeInTheDocument()
  })

  test('Return to Lobby button navigates to /lobby', () => {
    renderScreen()
    fireEvent.click(screen.getByRole('button', { name: /Return to Lobby/i }))
    expect(mockNavigate).toBeCalledWith('/lobby')
  })

  test('has game over dialog role for accessibility', () => {
    renderScreen()
    expect(screen.getByRole('dialog', { name: /Game over/i })).toBeInTheDocument()
  })
})
