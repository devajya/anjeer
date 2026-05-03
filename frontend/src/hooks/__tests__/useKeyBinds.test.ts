import { describe, test, expect, vi, beforeEach } from 'vitest'
import { renderHook, waitFor } from '@testing-library/react'
import { useKeyBinds, DEFAULT_BINDS } from '../useKeyBinds'

const mockFetch = vi.fn()
vi.stubGlobal('fetch', mockFetch)

beforeEach(() => {
  mockFetch.mockReset()
})

function okJson(data: unknown) {
  return Promise.resolve({ ok: true, json: () => Promise.resolve(data) })
}

function notOk(status = 401) {
  return Promise.resolve({ ok: false, status })
}

describe('useKeyBinds', () => {
  test('returns DEFAULT_BINDS when no DB binds are saved (empty array)', async () => {
    mockFetch.mockReturnValue(okJson([]))
    const { result } = renderHook(() => useKeyBinds())
    await waitFor(() => expect(result.current.loading).toBe(false))
    expect(result.current.binds).toEqual(DEFAULT_BINDS)
  })

  test('DB values override defaults for matching actions', async () => {
    mockFetch.mockReturnValue(okJson([{ action: 'suit_clubs', key_combo: '1' }]))
    const { result } = renderHook(() => useKeyBinds())
    await waitFor(() => expect(result.current.loading).toBe(false))
    expect(result.current.binds.suit_clubs).toBe('1')
    // Other defaults stay untouched
    expect(result.current.binds.suit_diamonds).toBe(DEFAULT_BINDS.suit_diamonds)
  })

  test('unknown DB actions are ignored and do not appear in binds', async () => {
    mockFetch.mockReturnValue(okJson([{ action: 'obsolete_action', key_combo: 'x' }]))
    const { result } = renderHook(() => useKeyBinds())
    await waitFor(() => expect(result.current.loading).toBe(false))
    expect('obsolete_action' in result.current.binds).toBe(false)
    expect(result.current.binds).toEqual(DEFAULT_BINDS)
  })

  test('returns DEFAULT_BINDS when fetch returns 401', async () => {
    mockFetch.mockReturnValue(notOk(401))
    const { result } = renderHook(() => useKeyBinds())
    await waitFor(() => expect(result.current.loading).toBe(false))
    expect(result.current.binds).toEqual(DEFAULT_BINDS)
  })

  test('returns DEFAULT_BINDS on network error', async () => {
    mockFetch.mockReturnValue(Promise.reject(new Error('network')))
    const { result } = renderHook(() => useKeyBinds())
    await waitFor(() => expect(result.current.loading).toBe(false))
    expect(result.current.binds).toEqual(DEFAULT_BINDS)
  })

  test('update() calls PUT /players/me/keybinds with the full bind list', async () => {
    mockFetch.mockReturnValue(okJson([]))
    const { result } = renderHook(() => useKeyBinds())
    await waitFor(() => expect(result.current.loading).toBe(false))

    const nextBinds = [{ action: 'suit_clubs', key_combo: '1' }]
    mockFetch.mockReturnValue(Promise.resolve({ ok: true }))
    await result.current.update(nextBinds)

    expect(mockFetch).toHaveBeenLastCalledWith('/players/me/keybinds', expect.objectContaining({
      method: 'PUT',
      body: JSON.stringify(nextBinds),
    }))
  })

  test('update() merges saved values into binds on next render', async () => {
    mockFetch.mockReturnValue(okJson([]))
    const { result } = renderHook(() => useKeyBinds())
    await waitFor(() => expect(result.current.loading).toBe(false))

    mockFetch.mockReturnValue(Promise.resolve({ ok: true }))
    await result.current.update([{ action: 'suit_spades', key_combo: '4' }])

    await waitFor(() => expect(result.current.binds.suit_spades).toBe('4'))
  })
})
