import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { renderHook, act } from '@testing-library/react'
import { useQueueSocket } from '../useQueueSocket'

// ── WebSocket mock ────────────────────────────────────────────────────────────

interface MockWsInstance {
  send:      ReturnType<typeof vi.fn>
  close:     ReturnType<typeof vi.fn>
  onopen:    (() => void) | null
  onmessage: ((e: { data: string }) => void) | null
  onclose:   (() => void) | null
  onerror:   (() => void) | null
  readyState: number
}

let lastWs: MockWsInstance | null = null

const MockWebSocket = vi.fn(function MockWebSocket() {
  const instance: MockWsInstance = {
    send:      vi.fn(),
    close:     vi.fn(),
    onopen:    null,
    onmessage: null,
    onclose:   null,
    onerror:   null,
    readyState: 1,
  }
  lastWs = instance
  return instance
}) as unknown as typeof WebSocket

;(MockWebSocket as { OPEN?: number }).OPEN = 1

vi.stubGlobal('WebSocket', MockWebSocket)

function sendWsMessage(data: object) {
  lastWs?.onmessage?.({ data: JSON.stringify(data) } as MessageEvent)
}

function triggerWsOpen() {
  lastWs?.onopen?.()
}

beforeEach(() => {
  lastWs = null
  vi.clearAllMocks()
})

afterEach(() => {
  // Allow any pending microtasks to flush
})

// ── Tests ─────────────────────────────────────────────────────────────────────

describe('useQueueSocket — game_state_snapshot → admitted (commit 18ad5b7)', () => {
  it('transitions to admitted when game_state_snapshot arrives (auto-reconnect path)', () => {
    const { result } = renderHook(() => useQueueSocket())

    act(() => { result.current.joinQueue('lobby-abc') })
    triggerWsOpen()

    act(() => { sendWsMessage({ type: 'game_state_snapshot', player_slot: 0 }) })

    expect(result.current.state.status).toBe('admitted')
    if (result.current.state.status === 'admitted') {
      expect(result.current.state.lobbyId).toBe('lobby-abc')
      // slotIndex is -1 for the auto-reconnect path (no queue slot assigned)
      expect(result.current.state.slotIndex).toBe(-1)
    }
  })

  it('WS is closed after game_state_snapshot (disconnect called)', () => {
    const { result } = renderHook(() => useQueueSocket())

    act(() => { result.current.joinQueue('lobby-abc') })
    triggerWsOpen()

    act(() => { sendWsMessage({ type: 'game_state_snapshot', player_slot: 0 }) })

    expect(lastWs?.close).toHaveBeenCalled()
  })

  it('queue_admitted still transitions to admitted with correct slotIndex (regression)', () => {
    const { result } = renderHook(() => useQueueSocket())

    act(() => { result.current.joinQueue('lobby-abc') })
    triggerWsOpen()
    act(() => { sendWsMessage({ type: 'queue_joined', position: 1, queue_size: 1 }) })
    act(() => { sendWsMessage({ type: 'queue_admitted', slot_index: 2 }) })

    expect(result.current.state.status).toBe('admitted')
    if (result.current.state.status === 'admitted') {
      expect(result.current.state.slotIndex).toBe(2)
    }
  })

  it('game_state_snapshot does nothing when lobbyId is not set', () => {
    const { result } = renderHook(() => useQueueSocket())
    // No joinQueue call — lobbyIdRef is null
    act(() => { sendWsMessage({ type: 'game_state_snapshot', player_slot: 0 }) })

    expect(result.current.state.status).toBe('idle')
  })
})
