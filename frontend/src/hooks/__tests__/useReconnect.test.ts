import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { renderHook, act } from '@testing-library/react'
import { useReconnect } from '../useReconnect'
import type { ClientCommand } from '../../types/messages'

// ── localStorage stub ────────────────────────────────────────────────────────
const lsStore: Record<string, string> = {}
vi.stubGlobal('localStorage', {
  getItem:    (k: string) => lsStore[k] ?? null,
  setItem:    (k: string, v: string) => { lsStore[k] = v },
  removeItem: (k: string) => { delete lsStore[k] },
  clear:      () => { Object.keys(lsStore).forEach(k => delete lsStore[k]) },
})

const LOBBY  = 'lobby_test'
const LS_KEY = `anjeer_reconnect_${LOBBY}`
const mockSend = vi.fn<[ClientCommand], void>()

beforeEach(() => {
  Object.keys(lsStore).forEach(k => delete lsStore[k])
  mockSend.mockClear()
  vi.useFakeTimers()
})

afterEach(() => {
  vi.useRealTimers()
})

describe('useReconnect', () => {
  it('stores token in localStorage on reconnect_token message', () => {
    const { result } = renderHook(() => useReconnect(LOBBY, 20, true, mockSend))
    act(() => {
      result.current.onTokenReceived('rtk_abc123', Date.now() + 20_000)
    })
    expect(lsStore[LS_KEY]).toContain('rtk_abc123')
  })

  it('status transitions to reconnecting on WS close', () => {
    const { result, rerender } = renderHook(
      ({ connected }: { connected: boolean }) => useReconnect(LOBBY, 20, connected, mockSend),
      { initialProps: { connected: true } },
    )
    act(() => { result.current.onTokenReceived('rtk_abc', Date.now() + 20_000) })
    expect(result.current.status).toBe('connected')

    // Wrap rerender in act so the connected-transition useEffect flushes
    act(() => { rerender({ connected: false }) })
    expect(result.current.status).toBe('reconnecting')
  })

  it('status transitions to reattached on game_state_snapshot', () => {
    const { result, rerender } = renderHook(
      ({ connected }: { connected: boolean }) => useReconnect(LOBBY, 20, connected, mockSend),
      { initialProps: { connected: true } },
    )
    act(() => { result.current.onTokenReceived('rtk_abc', Date.now() + 20_000) })
    act(() => { rerender({ connected: false }) })
    act(() => { result.current.onSnapshotReceived() })
    expect(result.current.status).toBe('reattached')
  })

  it('status transitions to expired when remainingSeconds reaches 0', () => {
    const { result, rerender } = renderHook(
      ({ connected }: { connected: boolean }) => useReconnect(LOBBY, 2, connected, mockSend),
      { initialProps: { connected: true } },
    )
    act(() => { result.current.onTokenReceived('rtk_abc', Date.now() + 20_000) })
    act(() => { rerender({ connected: false }) })
    expect(result.current.status).toBe('reconnecting')

    // Advance 3 s past the 2 s window
    act(() => { vi.advanceTimersByTime(3_000) })
    expect(result.current.status).toBe('expired')
  })

  it('clears localStorage token on expired', () => {
    const { result, rerender } = renderHook(
      ({ connected }: { connected: boolean }) => useReconnect(LOBBY, 1, connected, mockSend),
      { initialProps: { connected: true } },
    )
    act(() => { result.current.onTokenReceived('rtk_abc', Date.now() + 20_000) })
    expect(lsStore[LS_KEY]).toBeDefined()

    act(() => { rerender({ connected: false }) })
    expect(result.current.status).toBe('reconnecting')

    act(() => { vi.advanceTimersByTime(2_000) })
    expect(result.current.status).toBe('expired')
    expect(lsStore[LS_KEY]).toBeUndefined()
  })

  it('exponential backoff does not exceed window_seconds', () => {
    // Once the window expires, WS reconnection must NOT trigger another reconnect_game.
    const windowSeconds = 3
    const { result, rerender } = renderHook(
      ({ connected }: { connected: boolean }) => useReconnect(LOBBY, windowSeconds, connected, mockSend),
      { initialProps: { connected: true } },
    )
    act(() => { result.current.onTokenReceived('rtk_abc', Date.now() + windowSeconds * 1_000) })
    act(() => { rerender({ connected: false }) })
    expect(result.current.status).toBe('reconnecting')

    // Advance past the full window
    act(() => { vi.advanceTimersByTime((windowSeconds + 1) * 1_000) })
    expect(result.current.status).toBe('expired')

    // WS comes back — must NOT send reconnect_game (window closed)
    const sendsBefore = mockSend.mock.calls.length
    act(() => { rerender({ connected: true }) })
    expect(mockSend.mock.calls.length).toBe(sendsBefore)
    expect(result.current.status).toBe('expired')
  })
})
