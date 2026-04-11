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

  test('hook initialises with lastServerTs=null', () => {
    const { result } = renderHook(() => useWebSocket('/ws'))
    expect(result.current.lastServerTs).toBeNull()
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
      MockWebSocket.last.onerror?.(new Event('error'))
    })

    expect(result.current.connected).toBe(false)
  })
})

// ---------------------------------------------------------------------------
// AC: Server sends heartbeat — client displays last received timestamp
// ---------------------------------------------------------------------------

describe('useWebSocket — heartbeat messages', () => {
  test('updates lastServerTs on a valid heartbeat message', () => {
    const { result } = renderHook(() => useWebSocket('/ws'))

    act(() => { MockWebSocket.last.triggerOpen() })
    act(() => {
      MockWebSocket.last.triggerMessage({ type: 'heartbeat', server_ts: 1_700_000_000_000 })
    })

    expect(result.current.lastServerTs).toBe(1_700_000_000_000)
  })

  test('does not crash on unknown message type', () => {
    const { result } = renderHook(() => useWebSocket('/ws'))

    act(() => { MockWebSocket.last.triggerOpen() })
    expect(() => {
      act(() => {
        MockWebSocket.last.triggerMessage({ type: 'unknown_future_event', data: 42 })
      })
    }).not.toThrow()

    expect(result.current.lastServerTs).toBeNull()
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
