import { renderHook, act } from '@testing-library/react'
import { describe, test, expect, vi, beforeEach, afterEach } from 'vitest'
import { useWebSocket } from './useWebSocket'

// ---------------------------------------------------------------------------
// WebSocket mock
// AGENT-CTX: We replace the global WebSocket constructor so the hook's `new WebSocket()`
// call returns a controllable mock. Each test accesses the last created instance via
// MockWebSocket.last. The trigger* methods fire events on the hook as if they came
// from the real socket. Tests must call trigger* inside act() to flush React state.
// ---------------------------------------------------------------------------
class MockWebSocket {
  static last: MockWebSocket

  onopen: ((e: Event) => void) | null = null
  onclose: ((e: CloseEvent) => void) | null = null
  onerror: ((e: Event) => void) | null = null
  onmessage: ((e: MessageEvent) => void) | null = null
  readyState: number = 0 // CONNECTING

  // eslint-disable-next-line @typescript-eslint/no-unused-vars
  constructor(_url: string) {
    MockWebSocket.last = this
  }

  close() {
    this.readyState = 3 // CLOSED
  }

  triggerOpen() {
    this.readyState = 1 // OPEN
    this.onopen?.(new Event('open'))
  }

  triggerClose(code = 1000) {
    this.readyState = 3 // CLOSED
    this.onclose?.(new CloseEvent('close', { code }))
  }

  triggerMessage(data: unknown) {
    this.onmessage?.(new MessageEvent('message', { data: JSON.stringify(data) }))
  }
}

vi.stubGlobal('WebSocket', MockWebSocket)

beforeEach(() => {
  vi.clearAllMocks()
})

afterEach(() => {
  vi.restoreAllMocks()
})

// ---------------------------------------------------------------------------
// AC: React client connects via WebSocket on load
// ---------------------------------------------------------------------------

describe('useWebSocket — initial state', () => {
  test('hook initialises with connected=false', () => {
    const { result } = renderHook(() => useWebSocket('/ws'))
    expect(result.current.connected).toBe(false)
  })


})

describe('useWebSocket — connection lifecycle', () => {
  test('sets connected=true when WebSocket opens', () => {
    const { result } = renderHook(() => useWebSocket('/ws'))

    act(() => {
      MockWebSocket.last.triggerOpen()
    })

    expect(result.current.connected).toBe(true)
  })

  // AC: Disconnection is detected and displayed within 3 seconds (client-side half)
  test('sets connected=false when WebSocket closes', () => {
    const { result } = renderHook(() => useWebSocket('/ws'))

    act(() => { MockWebSocket.last.triggerOpen() })
    act(() => { MockWebSocket.last.triggerClose() })

    expect(result.current.connected).toBe(false)
  })

  test('sets connected=false on WebSocket error', () => {
    const { result } = renderHook(() => useWebSocket('/ws'))

    act(() => { MockWebSocket.last.triggerOpen() })
    act(() => {
      // Real browsers always fire onclose after onerror; mirror that here.
      MockWebSocket.last.onerror?.(new Event('error'))
      MockWebSocket.last.triggerClose()
    })

    expect(result.current.connected).toBe(false)
  })
})

// ---------------------------------------------------------------------------
// AC: Server sends book_update — books map is populated
// ---------------------------------------------------------------------------

describe('useWebSocket — book_update messages', () => {
  test('populates books on first book_update', () => {
    const { result } = renderHook(() => useWebSocket('/ws'))
    act(() => { MockWebSocket.last.triggerOpen() })
    act(() => {
      MockWebSocket.last.triggerMessage({ type: 'book_update', suit: 'S1', best_bid: 45, best_ask: 55 })
    })
    expect(result.current.books['S1']).toEqual({ best_bid: 45, best_ask: 55 })
  })

  test('updates only the affected suit', () => {
    const { result } = renderHook(() => useWebSocket('/ws'))
    act(() => { MockWebSocket.last.triggerOpen() })
    act(() => {
      MockWebSocket.last.triggerMessage({ type: 'book_update', suit: 'S1', best_bid: 40, best_ask: 60 })
      MockWebSocket.last.triggerMessage({ type: 'book_update', suit: 'S2', best_bid: 10, best_ask: 90 })
    })
    act(() => {
      MockWebSocket.last.triggerMessage({ type: 'book_update', suit: 'S1', best_bid: 42, best_ask: null })
    })
    expect(result.current.books['S1']).toEqual({ best_bid: 42, best_ask: null })
    expect(result.current.books['S2']).toEqual({ best_bid: 10, best_ask: 90 })
  })
})

// ---------------------------------------------------------------------------
// AC: order_ack — myOrders is populated; error is cleared for that suit
// ---------------------------------------------------------------------------

describe('useWebSocket — order_ack messages', () => {
  test('appends order to myOrders', () => {
    const { result } = renderHook(() => useWebSocket('/ws'))
    act(() => { MockWebSocket.last.triggerOpen() })
    act(() => {
      MockWebSocket.last.triggerMessage({
        type: 'order_ack', order_id: 7, suit: 'S1', side: 'buy', price: 50,
      })
    })
    expect(result.current.myOrders).toHaveLength(1)
    expect(result.current.myOrders[0]).toEqual({ order_id: 7, suit: 'S1', side: 'buy', price: 50 })
  })

  test('clears the error for that suit on ack', () => {
    const { result } = renderHook(() => useWebSocket('/ws'))
    act(() => { MockWebSocket.last.triggerOpen() })
    // Simulate a prior error on S1 by sending an error after a suit-scoped send
    act(() => {
      MockWebSocket.last.triggerMessage({ type: 'error', code: 'PRICE_OUT_OF_RANGE', message: 'bad price' })
    })
    act(() => {
      MockWebSocket.last.triggerMessage({
        type: 'order_ack', order_id: 8, suit: 'S1', side: 'sell', price: 55,
      })
    })
    expect(result.current.errors['S1']).toBeNull()
  })
})

// ---------------------------------------------------------------------------
// AC: trade — appended to feed, myOrders wiped (global wipe mechanic)
// ---------------------------------------------------------------------------

describe('useWebSocket — trade messages', () => {
  const INITIAL_HAND = { clubs: 3, diamonds: 2, hearts: 4, spades: 1 }

  function setupWithHand() {
    const { result } = renderHook(() => useWebSocket('/ws'))
    act(() => { MockWebSocket.last.triggerOpen() })
    act(() => {
      MockWebSocket.last.triggerMessage({
        type: 'round_start',
        player_slot: 0,
        hand: INITIAL_HAND,
        // AGENT-CTX: far-future timestamp keeps roundEndAt non-null for the
        // duration of these hand/trade tests without the timer firing.
        round_end_at: '2099-01-01T00:00:00.000Z',
      })
    })
    return result
  }

  test('increments hand count for traded suit when your_side=buy', () => {
    const result = setupWithHand()
    act(() => {
      MockWebSocket.last.triggerMessage({
        type: 'trade', suit: 'clubs', price: 50, aggressor_side: 'buy', your_side: 'buy',
      })
    })
    expect(result.current.hand?.clubs).toBe(INITIAL_HAND.clubs + 1)
    // other suits unchanged
    expect(result.current.hand?.diamonds).toBe(INITIAL_HAND.diamonds)
    expect(result.current.hand?.hearts).toBe(INITIAL_HAND.hearts)
  })

  test('decrements hand count for traded suit when your_side=sell', () => {
    const result = setupWithHand()
    act(() => {
      MockWebSocket.last.triggerMessage({
        type: 'trade', suit: 'spades', price: 30, aggressor_side: 'sell', your_side: 'sell',
      })
    })
    expect(result.current.hand?.spades).toBe(INITIAL_HAND.spades - 1)
  })

  test('does not change hand counts when your_side=null (observer)', () => {
    const result = setupWithHand()
    act(() => {
      MockWebSocket.last.triggerMessage({
        type: 'trade', suit: 'clubs', price: 50, aggressor_side: 'buy', your_side: null,
      })
    })
    expect(result.current.hand).toEqual(INITIAL_HAND)
  })

  test('initialHand is not mutated by trades', () => {
    const result = setupWithHand()
    act(() => {
      MockWebSocket.last.triggerMessage({
        type: 'trade', suit: 'clubs', price: 50, aggressor_side: 'buy', your_side: 'buy',
      })
    })
    expect(result.current.initialHand).toEqual(INITIAL_HAND)
    expect(result.current.hand?.clubs).toBe(INITIAL_HAND.clubs + 1)
  })

  test('appends to trades feed', () => {
    const { result } = renderHook(() => useWebSocket('/ws'))
    act(() => { MockWebSocket.last.triggerOpen() })
    act(() => {
      MockWebSocket.last.triggerMessage({
        type: 'trade', suit: 'S1', price: 50, aggressor_side: 'buy', your_side: null,
      })
    })
    expect(result.current.trades).toHaveLength(1)
    expect(result.current.trades[0].suit).toBe('S1')
    expect(result.current.trades[0].price).toBe(50)
  })

  test('clears myOrders on trade (global wipe)', () => {
    const { result } = renderHook(() => useWebSocket('/ws'))
    act(() => { MockWebSocket.last.triggerOpen() })
    act(() => {
      MockWebSocket.last.triggerMessage({
        type: 'order_ack', order_id: 1, suit: 'S1', side: 'buy', price: 45,
      })
    })
    expect(result.current.myOrders).toHaveLength(1)
    act(() => {
      MockWebSocket.last.triggerMessage({
        type: 'trade', suit: 'S1', price: 45, aggressor_side: 'sell', your_side: 'buy',
      })
    })
    expect(result.current.myOrders).toHaveLength(0)
  })

  test('newest trade appears first', () => {
    const { result } = renderHook(() => useWebSocket('/ws'))
    act(() => { MockWebSocket.last.triggerOpen() })
    act(() => {
      MockWebSocket.last.triggerMessage({
        type: 'trade', suit: 'S1', price: 40, aggressor_side: 'buy', your_side: null,
      })
    })
    act(() => {
      MockWebSocket.last.triggerMessage({
        type: 'trade', suit: 'S1', price: 60, aggressor_side: 'sell', your_side: null,
      })
    })
    expect(result.current.trades[0].price).toBe(60)
    expect(result.current.trades[1].price).toBe(40)
  })
})

// ---------------------------------------------------------------------------
// AC: order_cancel_ack — order is removed from myOrders
// ---------------------------------------------------------------------------

describe('useWebSocket — order_cancel_ack messages', () => {
  test('removes cancelled order from myOrders', () => {
    const { result } = renderHook(() => useWebSocket('/ws'))
    act(() => { MockWebSocket.last.triggerOpen() })
    act(() => {
      MockWebSocket.last.triggerMessage({ type: 'order_ack', order_id: 3, suit: 'S1', side: 'buy', price: 40 })
      MockWebSocket.last.triggerMessage({ type: 'order_ack', order_id: 4, suit: 'S1', side: 'sell', price: 60 })
    })
    act(() => {
      MockWebSocket.last.triggerMessage({ type: 'order_cancel_ack', order_id: 3 })
    })
    expect(result.current.myOrders).toHaveLength(1)
    expect(result.current.myOrders[0].order_id).toBe(4)
  })
})

// ---------------------------------------------------------------------------
// AC: error — scoped to the suit of the last sent command
// ---------------------------------------------------------------------------

describe('useWebSocket — error messages', () => {
  test('stores error under the suit of the last sent command', () => {
    const { result } = renderHook(() => useWebSocket('/ws'))
    act(() => { MockWebSocket.last.triggerOpen() })

    // Simulate send so pendingSuitRef is set to 'S2'
    act(() => {
      result.current.sendMessage({ type: 'submit_order', suit: 'S2', side: 'buy', price: 5 })
    })
    act(() => {
      MockWebSocket.last.triggerMessage({ type: 'error', code: 'PRICE_OUT_OF_RANGE', message: 'too low' })
    })

    expect(result.current.errors['S2']).toEqual({ type: 'error', code: 'PRICE_OUT_OF_RANGE', message: 'too low' })
    expect(result.current.errors['S1']).toBeUndefined()
  })

  test('errors for different suits are independent', () => {
    const { result } = renderHook(() => useWebSocket('/ws'))
    act(() => { MockWebSocket.last.triggerOpen() })

    act(() => { result.current.sendMessage({ type: 'nudge', suit: 'S1', side: 'buy' }) })
    act(() => { MockWebSocket.last.triggerMessage({ type: 'error', code: 'UNKNOWN_SUIT', message: 'bad suit' }) })

    act(() => { result.current.sendMessage({ type: 'nudge', suit: 'S2', side: 'sell' }) })
    act(() => { MockWebSocket.last.triggerMessage({ type: 'error', code: 'UNKNOWN_SUIT', message: 'bad suit' }) })

    expect(result.current.errors['S1']).not.toBeNull()
    expect(result.current.errors['S2']).not.toBeNull()
  })
})

// ---------------------------------------------------------------------------
// AC: round_start / round_end — roundEndAt state tracked correctly
// ---------------------------------------------------------------------------

describe('useWebSocket — round lifecycle', () => {
  test('round_start sets roundEndAt, playerSlot, and clears startsAt', () => {
    const { result } = renderHook(() => useWebSocket('/ws'))
    act(() => { MockWebSocket.last.triggerOpen() })
    // First receive round_starting so startsAt is set.
    act(() => {
      MockWebSocket.last.triggerMessage({
        type: 'round_starting', starts_at: '2026-04-15T20:00:00.000Z', player_count: 2,
      })
    })
    expect(result.current.startsAt).toBe('2026-04-15T20:00:00.000Z')

    act(() => {
      MockWebSocket.last.triggerMessage({
        type: 'round_start',
        player_slot: 2,
        hand: { clubs: 2, diamonds: 3, hearts: 1, spades: 2 },
        round_end_at: '2026-04-15T20:04:00.000Z',
      })
    })
    expect(result.current.roundEndAt).toBe('2026-04-15T20:04:00.000Z')
    expect(result.current.playerSlot).toBe(2)
    // round_start clears the pre-deal countdown.
    expect(result.current.startsAt).toBeNull()
  })

  test('round_end clears roundEndAt and stores the full result payload', () => {
    const { result } = renderHook(() => useWebSocket('/ws'))
    act(() => { MockWebSocket.last.triggerOpen() })
    act(() => {
      MockWebSocket.last.triggerMessage({
        type: 'round_start',
        player_slot: 0,
        hand: { clubs: 2, diamonds: 3, hearts: 1, spades: 2 },
        round_end_at: '2026-04-15T20:04:00.000Z',
      })
    })
    expect(result.current.roundEndAt).toBe('2026-04-15T20:04:00.000Z')

    act(() => {
      MockWebSocket.last.triggerMessage({
        type: 'round_end',
        goal_suit: 'hearts',
        results: [
          { player_slot: 0, goal_cards_held: 3, payout: 60, new_balance: 110, disconnected: false },
          { player_slot: 1, goal_cards_held: 1, payout: 20, new_balance: 70,  disconnected: false },
        ],
      })
    })
    // roundEndAt must be null after round_end so the active-round timer hides.
    expect(result.current.roundEndAt).toBeNull()
    expect(result.current.roundEnd).not.toBeNull()
    expect(result.current.roundEnd?.goal_suit).toBe('hearts')
    expect(result.current.roundEnd?.results).toHaveLength(2)
  })
})

describe('useWebSocket — unknown message type', () => {
  test('does not crash on unknown message type', () => {
    renderHook(() => useWebSocket('/ws'))

    act(() => { MockWebSocket.last.triggerOpen() })
    expect(() => {
      act(() => {
        MockWebSocket.last.triggerMessage({ type: 'unknown_future_event', data: 42 })
      })
    }).not.toThrow()
  })

  test('does not crash on malformed (non-JSON) message', () => {
    renderHook(() => useWebSocket('/ws'))

    act(() => { MockWebSocket.last.triggerOpen() })
    expect(() => {
      act(() => {
        MockWebSocket.last.onmessage?.(
          new MessageEvent('message', { data: 'not json {{' })
        )
      })
    }).not.toThrow()
  })
})
