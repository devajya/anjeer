import { render, screen, fireEvent } from '@testing-library/react'
import { describe, test, expect, vi } from 'vitest'
import { MyOrders } from './MyOrders'
import type { MyOrder } from '../hooks/useWebSocket'

const ORDER_BUY: MyOrder  = { order_id: 1, suit: 'S1', side: 'buy',  price: 45 }
const ORDER_SELL: MyOrder = { order_id: 2, suit: 'S2', side: 'sell', price: 55 }

describe('MyOrders — empty state', () => {
  test('shows placeholder when no orders', () => {
    render(<MyOrders orders={[]} onCancel={vi.fn()} />)
    expect(screen.getByText('No active orders.')).toBeInTheDocument()
  })
})

describe('MyOrders — order rendering', () => {
  test('shows side, suit, and price for each order', () => {
    render(<MyOrders orders={[ORDER_BUY]} onCancel={vi.fn()} />)
    expect(screen.getByText('BUY')).toBeInTheDocument()
    expect(screen.getByText('S1')).toBeInTheDocument()
    expect(screen.getByText('@ 45')).toBeInTheDocument()
  })

  test('renders multiple orders', () => {
    render(<MyOrders orders={[ORDER_BUY, ORDER_SELL]} onCancel={vi.fn()} />)
    expect(screen.getByText('BUY')).toBeInTheDocument()
    expect(screen.getByText('SELL')).toBeInTheDocument()
  })
})

describe('MyOrders — cancel interaction', () => {
  test('cancel button calls onCancel with the correct order_id', () => {
    const onCancel = vi.fn()
    render(<MyOrders orders={[ORDER_BUY, ORDER_SELL]} onCancel={onCancel} />)
    fireEvent.click(screen.getByRole('button', { name: /Cancel order 2/i }))
    expect(onCancel).toHaveBeenCalledOnce()
    expect(onCancel).toHaveBeenCalledWith(2)
  })

  test('each order has its own cancel button', () => {
    render(<MyOrders orders={[ORDER_BUY, ORDER_SELL]} onCancel={vi.fn()} />)
    expect(screen.getAllByRole('button')).toHaveLength(2)
  })
})
