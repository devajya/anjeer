import { render, screen } from '@testing-library/react'
import { describe, it, expect } from 'vitest'
import { AccumulationDisplay } from '../components/AccumulationDisplay'
import type { EvalAccumulationSignalMessage } from '../types/messages'

const MOCK_SIGNAL: EvalAccumulationSignalMessage = {
  type: 'eval_accumulation_signal',
  players: [
    { slot: 0, player: 'Alice', signal: 'Normal',   confidence: 0.2, primary_suit: 'spades'   },
    { slot: 1, player: 'Bob',   signal: 'Elevated', confidence: 0.7, primary_suit: 'hearts'   },
    { slot: 2, player: 'Carol', signal: 'High',     confidence: 0.9, primary_suit: 'diamonds' },
    { slot: 3, player: 'Dave',  signal: 'Normal',   confidence: 0.1, primary_suit: 'clubs'    },
  ],
}

describe('AccumulationDisplay', () => {
  it('renders empty state when no signal provided', () => {
    render(<AccumulationDisplay accumulationSignal={null} />)
    expect(screen.getByTestId('accumulation-display')).toBeInTheDocument()
    expect(screen.getByText(/Awaiting accumulation signal/i)).toBeInTheDocument()
  })

  it('renders a card for every player', () => {
    render(<AccumulationDisplay accumulationSignal={MOCK_SIGNAL} />)
    expect(screen.getByTestId('accumulation-card-0')).toBeInTheDocument()
    expect(screen.getByTestId('accumulation-card-1')).toBeInTheDocument()
    expect(screen.getByTestId('accumulation-card-2')).toBeInTheDocument()
    expect(screen.getByTestId('accumulation-card-3')).toBeInTheDocument()
  })

  it('Normal badge has gray class', () => {
    render(<AccumulationDisplay accumulationSignal={MOCK_SIGNAL} />)
    const badge = screen.getByTestId('accumulation-badge-0')
    expect(badge).toHaveClass('accumulation-display__badge--normal')
    expect(badge).not.toHaveClass('accumulation-display__badge--elevated')
    expect(badge).not.toHaveClass('accumulation-display__badge--high')
  })

  it('Elevated badge has amber class', () => {
    render(<AccumulationDisplay accumulationSignal={MOCK_SIGNAL} />)
    const badge = screen.getByTestId('accumulation-badge-1')
    expect(badge).toHaveClass('accumulation-display__badge--elevated')
    expect(badge).not.toHaveClass('accumulation-display__badge--normal')
    expect(badge).not.toHaveClass('accumulation-display__badge--high')
  })

  it('High badge has red class', () => {
    render(<AccumulationDisplay accumulationSignal={MOCK_SIGNAL} />)
    const badge = screen.getByTestId('accumulation-badge-2')
    expect(badge).toHaveClass('accumulation-display__badge--high')
    expect(badge).not.toHaveClass('accumulation-display__badge--normal')
    expect(badge).not.toHaveClass('accumulation-display__badge--elevated')
  })

  it('displays player names', () => {
    render(<AccumulationDisplay accumulationSignal={MOCK_SIGNAL} />)
    expect(screen.getByText('Alice')).toBeInTheDocument()
    expect(screen.getByText('Bob')).toBeInTheDocument()
    expect(screen.getByText('Carol')).toBeInTheDocument()
    expect(screen.getByText('Dave')).toBeInTheDocument()
  })

  it('confidence bar width reflects confidence value', () => {
    render(<AccumulationDisplay accumulationSignal={MOCK_SIGNAL} />)
    const bar = screen.getByTestId('accumulation-bar-1')
    expect(bar).toHaveStyle({ width: '70.0%' })
  })
})
