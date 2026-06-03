import { render, screen } from '@testing-library/react'
import { describe, test, expect } from 'vitest'
import { TradeFeed } from './TradeFeed'
import type { TradeEntry } from '../hooks/useWebSocket'

const ROSTER = [
  { player_slot: 0, username: 'Alice' },
  { player_slot: 1, username: 'Bob' },
]

function makeEntry(overrides: Partial<TradeEntry> = {}): TradeEntry {
  return {
    id: 1,
    suit: 'clubs',
    price: 50,
    aggressor_side: 'buy',
    your_side: null,
    buyer_slot: 0,
    seller_slot: 1,
    qty_filled: 1,
    qty_ordered: 1,
    ts: 0,
    ...overrides,
  }
}

describe('TradeFeed — empty state', () => {
  test('shows placeholder when no trades', () => {
    render(<TradeFeed trades={[]} roster={ROSTER} />)
    expect(screen.getByText('No trades yet.')).toBeInTheDocument()
  })

  test('does not render table when no trades', () => {
    const { container } = render(<TradeFeed trades={[]} roster={ROSTER} />)
    expect(container.querySelector('table')).toBeNull()
  })
})

describe('TradeFeed — header', () => {
  test('shows column headers when trades exist', () => {
    render(<TradeFeed trades={[makeEntry()]} roster={ROSTER} />)
    expect(screen.getByText('Buyer')).toBeInTheDocument()
    expect(screen.getByText('Suit')).toBeInTheDocument()
    expect(screen.getByText('Seller')).toBeInTheDocument()
    expect(screen.getByText('Price')).toBeInTheDocument()
  })
})

describe('TradeFeed — trade row rendering', () => {
  test('shows buyer username from roster', () => {
    render(<TradeFeed trades={[makeEntry({ buyer_slot: 0 })]} roster={ROSTER} />)
    expect(screen.getByText('Alice')).toBeInTheDocument()
  })

  test('shows seller username from roster', () => {
    render(<TradeFeed trades={[makeEntry({ seller_slot: 1 })]} roster={ROSTER} />)
    expect(screen.getByText('Bob')).toBeInTheDocument()
  })

  test('shows suit symbol for clubs', () => {
    render(<TradeFeed trades={[makeEntry({ suit: 'clubs' })]} roster={ROSTER} />)
    expect(screen.getByText('♣')).toBeInTheDocument()
  })

  test('shows suit symbol for diamonds', () => {
    render(<TradeFeed trades={[makeEntry({ suit: 'diamonds' })]} roster={ROSTER} />)
    expect(screen.getByText('♦')).toBeInTheDocument()
  })

  test('shows suit symbol for hearts', () => {
    render(<TradeFeed trades={[makeEntry({ suit: 'hearts' })]} roster={ROSTER} />)
    expect(screen.getByText('♥')).toBeInTheDocument()
  })

  test('shows suit symbol for spades', () => {
    render(<TradeFeed trades={[makeEntry({ suit: 'spades' })]} roster={ROSTER} />)
    expect(screen.getByText('♠')).toBeInTheDocument()
  })

  test('shows price', () => {
    render(<TradeFeed trades={[makeEntry({ price: 72 })]} roster={ROSTER} />)
    expect(screen.getByText('72')).toBeInTheDocument()
  })

  test('falls back to "Slot N" when slot not in roster', () => {
    render(<TradeFeed trades={[makeEntry({ buyer_slot: 99 })]} roster={ROSTER} />)
    expect(screen.getByText('Slot 99')).toBeInTheDocument()
  })

  test('does not crash when roster is empty', () => {
    render(<TradeFeed trades={[makeEntry()]} roster={[]} />)
    expect(screen.getByText('Slot 0')).toBeInTheDocument()
    expect(screen.getByText('Slot 1')).toBeInTheDocument()
  })
})

describe('TradeFeed — mine highlighting', () => {
  test('applies --mine class when your_side is non-null', () => {
    const { container } = render(
      <TradeFeed trades={[makeEntry({ your_side: 'buy' })]} roster={ROSTER} />
    )
    expect(container.querySelector('.trade-feed__row--mine')).not.toBeNull()
  })

  test('does not apply --mine class when your_side is null', () => {
    const { container } = render(
      <TradeFeed trades={[makeEntry({ your_side: null })]} roster={ROSTER} />
    )
    expect(container.querySelector('.trade-feed__row--mine')).toBeNull()
  })
})

describe('TradeFeed — partial fill display', () => {
  test('TradeFeed renders partial fill as "X filled of Y"', () => {
    render(<TradeFeed trades={[makeEntry({ qty_filled: 3, qty_ordered: 5 })]} roster={ROSTER} />)
    expect(screen.getByText('3/5')).toBeInTheDocument()
  })

  test('full fill (1/1) shows no qty ratio', () => {
    const { container } = render(
      <TradeFeed trades={[makeEntry({ qty_filled: 1, qty_ordered: 1 })]} roster={ROSTER} />
    )
    expect(container.querySelector('.trade-feed__qty')).toBeNull()
  })

  test('full fill of qty > 1 shows no qty ratio', () => {
    const { container } = render(
      <TradeFeed trades={[makeEntry({ qty_filled: 3, qty_ordered: 3 })]} roster={ROSTER} />
    )
    expect(container.querySelector('.trade-feed__qty')).toBeNull()
  })
})

describe('TradeFeed — multiple trades', () => {
  test('renders all trade rows', () => {
    const trades = [
      makeEntry({ id: 1, price: 50, buyer_slot: 0, seller_slot: 1 }),
      makeEntry({ id: 2, price: 63, buyer_slot: 1, seller_slot: 0 }),
    ]
    render(<TradeFeed trades={trades} roster={ROSTER} />)
    expect(screen.getByText('50')).toBeInTheDocument()
    expect(screen.getByText('63')).toBeInTheDocument()
  })
})
