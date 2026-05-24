import { render, screen } from '@testing-library/react'
import { describe, it, expect } from 'vitest'
import { PosteriorDisplay } from '../components/PosteriorDisplay'
import { SettlementEV } from '../components/SettlementEV'
import type { EvalPosteriorUpdateMessage } from '../types/messages'

const MOCK_POSTERIOR: EvalPosteriorUpdateMessage = {
  type: 'eval_posterior_update',
  player_slot: 0,
  configurations: [
    { deck_index: 0, counts: [12, 10, 8, 8], goal_suit: 'spades',   probability: 0.58 },
    { deck_index: 1, counts: [10, 12, 8, 8], goal_suit: 'clubs',    probability: 0.25 },
    { deck_index: 2, counts: [8, 8, 12, 10], goal_suit: 'hearts',   probability: 0.10 },
    { deck_index: 3, counts: [8, 8, 8, 14],  goal_suit: 'diamonds', probability: 0.07 },
  ],
  goal_suit_marginals: { clubs: 0.25, diamonds: 0.07, hearts: 0.10, spades: 0.58 },
  settlement_ev: 174.0,
  delta_ev: { clubs: 12.5, diamonds: -3.5, hearts: 5.0, spades: 29.0 },
}

// ── PosteriorDisplay ──────────────────────────────────────────────────────────

describe('PosteriorDisplay', () => {
  it('renders placeholder when posteriorUpdate is null', () => {
    render(<PosteriorDisplay posteriorUpdate={null} />)
    expect(screen.getByTestId('posterior-display')).toBeInTheDocument()
    expect(screen.getByText(/awaiting bayesian update/i)).toBeInTheDocument()
  })

  it('renders exactly 3 top configs from a 4-entry list', () => {
    render(<PosteriorDisplay posteriorUpdate={MOCK_POSTERIOR} />)
    const configs = screen.getByTestId('posterior-configs')
    // top-3 configs present; 4th (diamonds 7%) absent
    expect(configs.querySelectorAll('[data-testid^="posterior-config-"]')).toHaveLength(3)
  })

  it('displays configs sorted by probability descending', () => {
    render(<PosteriorDisplay posteriorUpdate={MOCK_POSTERIOR} />)
    const bars = screen.getAllByTestId(/^posterior-bar-/)
    // First bar should be deck 0 (58%)
    expect(screen.getByTestId('posterior-config-0')).toBeInTheDocument()
    // Second should be deck 1 (25%)
    expect(screen.getByTestId('posterior-config-1')).toBeInTheDocument()
    // Third should be deck 2 (10%)
    expect(screen.getByTestId('posterior-config-2')).toBeInTheDocument()
    expect(bars).toHaveLength(3)
  })

  it('renders 4 marginal cells (one per suit)', () => {
    render(<PosteriorDisplay posteriorUpdate={MOCK_POSTERIOR} />)
    const marginals = screen.getByTestId('posterior-marginals')
    expect(marginals.children).toHaveLength(4)
  })

  it('shows correct spades marginal probability', () => {
    render(<PosteriorDisplay posteriorUpdate={MOCK_POSTERIOR} />)
    // 58% → displayed as "58%"
    expect(screen.getByTestId('marginal-spades').textContent).toBe('58%')
  })

  it('shows correct clubs marginal probability', () => {
    render(<PosteriorDisplay posteriorUpdate={MOCK_POSTERIOR} />)
    expect(screen.getByTestId('marginal-clubs').textContent).toBe('25%')
  })

  it('rotates displayed configs when a previously 4th-ranked deck overtakes one in the top-3', () => {
    const { rerender } = render(<PosteriorDisplay posteriorUpdate={MOCK_POSTERIOR} />)
    // Initial top-3: deck 0 (58%), deck 1 (25%), deck 2 (10%). Deck 3 (7%) is absent.
    expect(screen.getByTestId('posterior-config-0')).toBeInTheDocument()
    expect(screen.getByTestId('posterior-config-1')).toBeInTheDocument()
    expect(screen.getByTestId('posterior-config-2')).toBeInTheDocument()
    expect(screen.queryByTestId('posterior-config-3')).not.toBeInTheDocument()

    // Shift: deck 3 overtakes deck 2 (diamonds 40% > hearts 5%)
    const shifted: EvalPosteriorUpdateMessage = {
      ...MOCK_POSTERIOR,
      configurations: [
        { deck_index: 0, counts: [12, 10, 8, 8], goal_suit: 'spades',   probability: 0.40 },
        { deck_index: 1, counts: [10, 12, 8, 8], goal_suit: 'clubs',    probability: 0.30 },
        { deck_index: 2, counts: [8, 8, 12, 10], goal_suit: 'hearts',   probability: 0.05 },
        { deck_index: 3, counts: [8, 8, 8, 14],  goal_suit: 'diamonds', probability: 0.25 },
      ],
    }
    rerender(<PosteriorDisplay posteriorUpdate={shifted} />)

    // After shift: top-3 is deck 0 (40%), deck 1 (30%), deck 3 (25%). Deck 2 (5%) drops out.
    expect(screen.getByTestId('posterior-config-0')).toBeInTheDocument()
    expect(screen.getByTestId('posterior-config-1')).toBeInTheDocument()
    expect(screen.getByTestId('posterior-config-3')).toBeInTheDocument()
    expect(screen.queryByTestId('posterior-config-2')).not.toBeInTheDocument()
  })
})

// ── SettlementEV ──────────────────────────────────────────────────────────────

describe('SettlementEV', () => {
  it('renders placeholder when posteriorUpdate is null', () => {
    render(<SettlementEV posteriorUpdate={null} />)
    expect(screen.getByTestId('settlement-ev')).toBeInTheDocument()
    expect(screen.getByText(/no ev estimate/i)).toBeInTheDocument()
  })

  it('shows settlement EV value from mock data', () => {
    render(<SettlementEV posteriorUpdate={MOCK_POSTERIOR} />)
    expect(screen.getByTestId('settlement-ev-total').textContent).toBe('174.0')
  })

  it('shows 4 delta EV cells', () => {
    render(<SettlementEV posteriorUpdate={MOCK_POSTERIOR} />)
    const deltas = screen.getByTestId('settlement-ev-deltas')
    expect(deltas.children).toHaveLength(4)
  })

  it('prefixes positive delta EV with +', () => {
    render(<SettlementEV posteriorUpdate={MOCK_POSTERIOR} />)
    // spades delta_ev = 29.0 → "+29.0"
    expect(screen.getByTestId('delta-ev-spades').textContent).toBe('+29.0')
  })

  it('shows negative delta EV without + prefix', () => {
    render(<SettlementEV posteriorUpdate={MOCK_POSTERIOR} />)
    // diamonds delta_ev = -3.5 → "-3.5"
    expect(screen.getByTestId('delta-ev-diamonds').textContent).toBe('-3.5')
  })

  it('applies positive color class to gain suits', () => {
    render(<SettlementEV posteriorUpdate={MOCK_POSTERIOR} />)
    expect(screen.getByTestId('delta-ev-clubs')).toHaveClass('settlement-ev__delta-val--pos')
  })

  it('applies negative color class to loss suits', () => {
    render(<SettlementEV posteriorUpdate={MOCK_POSTERIOR} />)
    expect(screen.getByTestId('delta-ev-diamonds')).toHaveClass('settlement-ev__delta-val--neg')
  })
})
