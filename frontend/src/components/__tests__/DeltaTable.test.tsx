import { render, screen } from '@testing-library/react'
import { describe, test, expect } from 'vitest'
import { DeltaTable } from '../DeltaTable'

const ROSTER = [
  { player_slot: 0, username: 'Alice' },
  { player_slot: 1, username: 'Bob' },
  { player_slot: 2, username: 'Carol' },
]

// deltas[slot][suit]: 0=clubs 1=diamonds 2=hearts 3=spades
const ZERO_DELTAS: number[][] = [
  [0, 0, 0, 0],
  [0, 0, 0, 0],
  [0, 0, 0, 0],
]

describe('DeltaTable — renders nothing without roster', () => {
  test('returns null when roster is empty', () => {
    const { container } = render(<DeltaTable deltas={[]} roster={[]} mySlot={0} />)
    expect(container.firstChild).toBeNull()
  })
})

describe('DeltaTable — column headers', () => {
  test('renders one column per player in roster', () => {
    render(<DeltaTable deltas={ZERO_DELTAS} roster={ROSTER} mySlot={0} />)
    expect(screen.getByText('Alice')).toBeInTheDocument()
    expect(screen.getByText('Bob')).toBeInTheDocument()
    expect(screen.getByText('Carol')).toBeInTheDocument()
  })

  test('marks mySlot column with "you" badge', () => {
    render(<DeltaTable deltas={ZERO_DELTAS} roster={ROSTER} mySlot={1} />)
    expect(screen.getByText('you')).toBeInTheDocument()
    // Only one badge regardless of player count
    expect(screen.getAllByText('you')).toHaveLength(1)
  })

  test('no "you" badge when mySlot is null', () => {
    render(<DeltaTable deltas={ZERO_DELTAS} roster={ROSTER} mySlot={null} />)
    expect(screen.queryByText('you')).toBeNull()
  })
})

describe('DeltaTable — suit rows', () => {
  test('renders four suit symbols', () => {
    render(<DeltaTable deltas={ZERO_DELTAS} roster={ROSTER} mySlot={0} />)
    expect(screen.getByText('♣')).toBeInTheDocument()
    expect(screen.getByText('♦')).toBeInTheDocument()
    expect(screen.getByText('♥')).toBeInTheDocument()
    expect(screen.getByText('♠')).toBeInTheDocument()
  })
})

describe('DeltaTable — delta values', () => {
  test('shows — for zero delta', () => {
    render(<DeltaTable deltas={ZERO_DELTAS} roster={ROSTER} mySlot={0} />)
    const dashes = screen.getAllByText('—')
    // 4 suits × 3 players = 12 zero cells
    expect(dashes.length).toBe(12)
  })

  test('shows ▲2 for +2 clubs delta for slot 0', () => {
    const deltas = [
      [2, 0, 0, 0],
      [0, 0, 0, 0],
      [0, 0, 0, 0],
    ]
    render(<DeltaTable deltas={deltas} roster={ROSTER} mySlot={0} />)
    expect(screen.getByText('▲2')).toBeInTheDocument()
  })

  test('shows ▼1 for -1 spades delta for slot 1', () => {
    const deltas = [
      [0, 0, 0, 0],
      [0, 0, 0, -1],
      [0, 0, 0, 0],
    ]
    render(<DeltaTable deltas={deltas} roster={ROSTER} mySlot={0} />)
    expect(screen.getByText('▼1')).toBeInTheDocument()
  })

  test('handles missing deltas row gracefully (treats as zero)', () => {
    // Only one slot's data provided — others should show —
    render(<DeltaTable deltas={[[1, 0, 0, 0]]} roster={ROSTER} mySlot={0} />)
    expect(screen.getByText('▲1')).toBeInTheDocument()
    // Slots 1 and 2 have no row → getDelta returns 0 → shown as —
    const dashes = screen.getAllByText('—')
    expect(dashes.length).toBeGreaterThan(0)
  })
})

describe('DeltaTable — my column styling', () => {
  test('my column header gets --me class', () => {
    const { container } = render(
      <DeltaTable deltas={ZERO_DELTAS} roster={ROSTER} mySlot={0} />
    )
    expect(
      container.querySelector('.delta-table__col-header--me')
    ).not.toBeNull()
  })

  test('my cells get --me class', () => {
    const { container } = render(
      <DeltaTable deltas={ZERO_DELTAS} roster={ROSTER} mySlot={0} />
    )
    const meCells = container.querySelectorAll('.delta-table__cell--me')
    // 4 suit rows × 1 my column = 4 cells
    expect(meCells.length).toBe(4)
  })
})
