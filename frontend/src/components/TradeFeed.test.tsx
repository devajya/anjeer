import { render, screen } from '@testing-library/react'
import { describe, test, expect } from 'vitest'
import { TradeFeed } from './TradeFeed'
import type { TradeEntry } from '../hooks/useWebSocket'

function makeEntry(overrides: Partial<TradeEntry> = {}): TradeEntry {
  return {
    id: 1,
    suit: 'S1',
    price: 50,
    aggressor_side: 'buy',
    your_side: null,
    ts: 0,
    ...overrides,
  }
}

describe('TradeFeed — empty state', () => {
  test('shows placeholder when no trades', () => {
    render(<TradeFeed trades={[]} />)
    expect(screen.getByText('No trades yet.')).toBeInTheDocument()
  })
})

describe('TradeFeed — trade rendering', () => {
  test('shows suit and price', () => {
    render(<TradeFeed trades={[makeEntry({ suit: 'S2', price: 72 })]} />)
    expect(screen.getByText('S2')).toBeInTheDocument()
    expect(screen.getByText('@ 72')).toBeInTheDocument()
  })

  test('shows aggressor side in uppercase', () => {
    render(<TradeFeed trades={[makeEntry({ aggressor_side: 'sell' })]} />)
    expect(screen.getByText('SELL')).toBeInTheDocument()
  })

  test('does not show "you" label when your_side is null', () => {
    render(<TradeFeed trades={[makeEntry({ your_side: null })]} />)
    expect(screen.queryByText(/you/)).toBeNull()
  })

  test('shows "you buy" label when your_side is buy', () => {
    render(<TradeFeed trades={[makeEntry({ your_side: 'buy' })]} />)
    expect(screen.getByText('you buy')).toBeInTheDocument()
  })

  test('shows "you sell" label when your_side is sell', () => {
    render(<TradeFeed trades={[makeEntry({ your_side: 'sell' })]} />)
    expect(screen.getByText('you sell')).toBeInTheDocument()
  })

  test('applies mine class when your_side is non-null', () => {
    const { container } = render(<TradeFeed trades={[makeEntry({ your_side: 'buy' })]} />)
    expect(container.querySelector('.trade-feed__item--mine')).not.toBeNull()
  })

  test('does not apply mine class when your_side is null', () => {
    const { container } = render(<TradeFeed trades={[makeEntry({ your_side: null })]} />)
    expect(container.querySelector('.trade-feed__item--mine')).toBeNull()
  })

  test('renders multiple trades', () => {
    const trades = [
      makeEntry({ id: 1, price: 50 }),
      makeEntry({ id: 2, price: 60 }),
    ]
    render(<TradeFeed trades={trades} />)
    expect(screen.getByText('@ 50')).toBeInTheDocument()
    expect(screen.getByText('@ 60')).toBeInTheDocument()
  })
})
