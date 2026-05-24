import { render, screen, waitFor, fireEvent, act } from '@testing-library/react'
import { describe, test, expect, vi, beforeEach, afterAll } from 'vitest'
import { MemoryRouter, Route, Routes } from 'react-router-dom'
import { LobbyRoom } from '../LobbyRoom'
import { useWebSocket } from '../../hooks/useWebSocket'
import type { UseWebSocketReturn } from '../../hooks/useWebSocket'
import type { LobbyStateMessage } from '../../types/messages'

// AGENT-CTX: useWebSocket is mocked at module level so tests control lobby state
// without spinning up a real WebSocket. fetch is mocked to return the lobby-by-code
// resolution response (GET /lobbies → find by :code param).
// useAuth is mocked to provide a stable user identity for owner-check tests.

const mockNavigate  = vi.fn()
const mockSubscribe = vi.fn()
const mockUnsub     = vi.fn()
const mockSendMsg   = vi.fn()

vi.mock('react-router-dom', async (importOriginal) => {
  const actual = await importOriginal<typeof import('react-router-dom')>()
  return { ...actual, useNavigate: () => mockNavigate }
})

vi.mock('../../hooks/useAuth', () => ({
  useAuth: () => ({ user: { id: 1, username: 'owner', games_played: 0 }, logout: vi.fn() })
}))

vi.mock('../../hooks/useWebSocket', () => ({
  useWebSocket: vi.fn(),
}))

// ─── Helpers ──────────────────────────────────────────────────────────────────

const BASE_LOBBY_STATE: LobbyStateMessage = {
  type: 'lobby_state',
  lobby_id: 'lobby-uuid-1',
  code: 'ABC123',
  creator_id: 1,            // matches user.id=1 → this user is the owner
  status: 'waiting',
  min_players: 2,
  max_players: 8,
  mode: 'ui',
  spawn_bots_on_leave: false,
  bot_spawn_difficulty: 'easy',
  players: [
    { player_id: 1, username: 'owner', joined_at: '2026-04-23T00:00:00Z', is_bot: false },
    { player_id: 2, username: 'alice', joined_at: '2026-04-23T00:00:00Z', is_bot: false },
  ],
}

function makeWsReturn(overrides: Partial<UseWebSocketReturn> = {}): UseWebSocketReturn {
  return {
    connected:         true,
    playerId:          1,
    books:             {},
    trades:            [],
    errors:            {},
    myOrders:          [],
    startsAt:          null,
    hand:              null,
    initialHand:       null,
    playerSlot:        null,
    roundEndAt:        null,
    roundEnd:          null,
    balance:           null,
    waitingForStart:   null,
    lobbyState:        BASE_LOBBY_STATE,
    lobbyStarted:      null,
    interRound:        null,
    gameEnded:         null,
    sessionError:      null,
    departedSlots:     [],
    deltas:            [],
    roster:            [],
    allBalances:       [],
    allHandTotals:     [],
    sendMessage:       mockSendMsg,
    subscribeLobby:    mockSubscribe,
    unsubscribeLobby:  mockUnsub,
    joinQueue:         vi.fn(),
    leaveQueue:        vi.fn(),
    resetQueue:        vi.fn(),
    ownsBestBidBySuit: {},
    ownsBestAskBySuit: {},
    spectatorCount:    0,
    scriptLogs:        [],
    reconnectTokenMsg:      null,
    gameStateSnapshot:      null,
    reconnectWindowExpired: false,
    queueState:             { status: 'idle' },
    currentOwnerPlayerId:   null,
    currentOwnerUsername:   '',
    evalPosteriorUpdate:    null,
    evalAccumulationSignal: null,
    evalExecutionGuidance:  null,
    ...overrides,
  }
}

// Fetch mock: GET /lobbies returns the ABC123 lobby so lobbyId resolution succeeds.
function setupFetch() {
  vi.stubGlobal('fetch', vi.fn().mockResolvedValue({
    ok:   true,
    json: () => Promise.resolve({
      lobbies: [{
        id:           'lobby-uuid-1',
        code:         'ABC123',
        creator_id:     1,
        status:       'waiting',
        min_players:  2,
        max_players:  8,
        player_count: 2,
        created_at:   '2026-04-23T00:00:00Z',
      }],
    }),
  }))
}

function renderRoom() {
  return render(
    <MemoryRouter initialEntries={['/lobby/ABC123']}>
      <Routes>
        <Route path="/lobby/:code" element={<LobbyRoom />} />
      </Routes>
    </MemoryRouter>
  )
}

beforeEach(() => {
  vi.clearAllMocks()
  setupFetch()
  vi.mocked(useWebSocket).mockReturnValue(makeWsReturn())
})

// ─── Task 9: leave_lobby on back-nav ─────────────────────────────────────────

describe('LobbyRoom — leave_lobby on back-nav', () => {
  test('back button sends leave_lobby then navigates to /lobby', async () => {
    renderRoom()

    // Wait for lobbyId resolution (fetch) and lobby state render
    await waitFor(() => screen.getByText('← Lobbies'))

    fireEvent.click(screen.getByText('← Lobbies'))

    expect(mockSendMsg).toHaveBeenCalledWith({
      type: 'leave_lobby',
      lobby_id: 'lobby-uuid-1',
    })
    expect(mockNavigate).toHaveBeenCalledWith('/lobby')
  })

  test('lobby_started navigation does not send leave_lobby', async () => {
    // AGENT-CTX: When lobby_started fires, navigatedToGameRef is set to true so
    // neither handleBack nor the unmount cleanup sends leave_lobby.
    vi.mocked(useWebSocket).mockReturnValue(makeWsReturn({
      lobbyStarted: { type: 'lobby_started', lobby_id: 'lobby-uuid-1', code: 'ABC123' },
    }))

    const { unmount } = renderRoom()

    await waitFor(() => {
      expect(mockNavigate).toHaveBeenCalledWith('/game?lobby_id=lobby-uuid-1')
    })

    unmount()

    expect(mockSendMsg).not.toHaveBeenCalledWith(
      expect.objectContaining({ type: 'leave_lobby' })
    )
  })
})

// ─── AC4: Start button visibility ────────────────────────────────────────────

describe('LobbyRoom — start button', () => {
  test('start button visible only for lobby owner with enough players', async () => {
    // owner_id=1 matches user.id=1; 2 players ≥ min_players=2
    renderRoom()

    await waitFor(() => {
      expect(screen.getByText('Start')).toBeInTheDocument()
    })
  })

  test('start button hidden for non-owner', async () => {
    // owner_id=99 — does not match user.id=1
    vi.mocked(useWebSocket).mockReturnValue(makeWsReturn({
      lobbyState: { ...BASE_LOBBY_STATE, creator_id: 99 },
    }))

    renderRoom()

    await waitFor(() => screen.getByText('owner'))
    expect(screen.queryByText('Start Game')).not.toBeInTheDocument()
  })
})

// ─── AC5: Real-time membership events ────────────────────────────────────────

describe('LobbyRoom — membership events', () => {
  test('player_joined event adds player to list', async () => {
    // Initial: only owner present
    vi.mocked(useWebSocket).mockReturnValue(makeWsReturn({
      lobbyState: {
        ...BASE_LOBBY_STATE,
        players: [{ player_id: 1, username: 'owner', joined_at: '', is_bot: false }],
      },
    }))

    const { rerender } = renderRoom()

    await waitFor(() => screen.getByText('owner'))
    expect(screen.queryByText('alice')).not.toBeInTheDocument()

    // Simulate player_joined updating lobbyState inside useWebSocket
    vi.mocked(useWebSocket).mockReturnValue(makeWsReturn({
      lobbyState: {
        ...BASE_LOBBY_STATE,
        players: [
          { player_id: 1, username: 'owner', joined_at: '', is_bot: false },
          { player_id: 2, username: 'alice', joined_at: '', is_bot: false },
        ],
      },
    }))

    rerender(
      <MemoryRouter initialEntries={['/lobby/ABC123']}>
        <Routes>
          <Route path="/lobby/:code" element={<LobbyRoom />} />
        </Routes>
      </MemoryRouter>
    )

    expect(screen.getByText('alice')).toBeInTheDocument()
  })

  test('player_left event removes player from list', async () => {
    // Initial: both players present (BASE_LOBBY_STATE)
    const { rerender } = renderRoom()

    await waitFor(() => screen.getByText('alice'))

    // Simulate player_left removing alice from lobbyState
    vi.mocked(useWebSocket).mockReturnValue(makeWsReturn({
      lobbyState: {
        ...BASE_LOBBY_STATE,
        players: [{ player_id: 1, username: 'owner', joined_at: '', is_bot: false }],
      },
    }))

    rerender(
      <MemoryRouter initialEntries={['/lobby/ABC123']}>
        <Routes>
          <Route path="/lobby/:code" element={<LobbyRoom />} />
        </Routes>
      </MemoryRouter>
    )

    expect(screen.queryByText('alice')).not.toBeInTheDocument()
    expect(screen.getByText('owner')).toBeInTheDocument()
  })

  test('lobby_started event navigates to /game', async () => {
    // AGENT-CTX: lobbyStarted.lobby_id must match lobbyId resolved from fetch.
    // The useEffect guard in LobbyRoom compares these to avoid spurious navigation.
    vi.mocked(useWebSocket).mockReturnValue(makeWsReturn({
      lobbyStarted: { type: 'lobby_started', lobby_id: 'lobby-uuid-1', code: 'ABC123' },
    }))

    renderRoom()

    await waitFor(() => {
      expect(mockNavigate).toHaveBeenCalledWith('/game?lobby_id=lobby-uuid-1')
    })
  })
})

// ─── T17–T19: Bot controls ────────────────────────────────────────────────────

const BOT_PLAYER = {
  player_id: null,
  bot_uuid: 'bot-uuid-1',
  username: 'EasyBot #1',
  is_bot: true,
  bot_difficulty: 'easy' as const,
  joined_at: '',
}

describe('LobbyRoom — bot controls', () => {
  test('T17: renders bot difficulty toggle for is_bot player in lobby_state', async () => {
    vi.mocked(useWebSocket).mockReturnValue(makeWsReturn({
      lobbyState: {
        ...BASE_LOBBY_STATE,
        players: [
          { player_id: 1, username: 'owner', joined_at: '', is_bot: false },
          BOT_PLAYER,
        ],
      },
    }))

    renderRoom()

    await waitFor(() => {
      // Bot seat shows a per-seat difficulty segmented toggle; "easy" segment is active
      expect(screen.getByRole('button', { name: 'easy' })).toBeInTheDocument()
    })
    expect(screen.getByText('EasyBot #1')).toBeInTheDocument()
  })

  test('T18: lobby owner sees clickable empty seats when slots available', async () => {
    // creator_id=1 matches user.id=1; 2 players, max=8 → 6 empty seats clickable
    renderRoom()

    await waitFor(() => {
      expect(screen.getAllByRole('button', { name: /add bot to seat/i }).length).toBeGreaterThan(0)
    })
  })

  test('T19: non-owner does not see clickable empty seats', async () => {
    vi.mocked(useWebSocket).mockReturnValue(makeWsReturn({
      lobbyState: { ...BASE_LOBBY_STATE, creator_id: 99 },
    }))

    renderRoom()

    await waitFor(() => screen.getByText('owner'))
    expect(screen.queryByRole('button', { name: /add bot to seat/i })).toBeNull()
  })

  test('clicking empty seat then difficulty sends add_bot message', async () => {
    renderRoom()

    await waitFor(() => screen.getAllByRole('button', { name: /add bot to seat/i }))
    // Click the first available empty seat
    fireEvent.click(screen.getAllByRole('button', { name: /add bot to seat/i })[0])

    await waitFor(() => screen.getByRole('button', { name: /easy/i }))
    fireEvent.click(screen.getByRole('button', { name: /easy/i }))

    expect(mockSendMsg).toHaveBeenCalledWith({
      type: 'add_bot',
      lobby_id: 'lobby-uuid-1',
      difficulty: 'easy',
    })
  })

  test('clicking remove sends remove_bot message', async () => {
    vi.mocked(useWebSocket).mockReturnValue(makeWsReturn({
      lobbyState: {
        ...BASE_LOBBY_STATE,
        players: [
          { player_id: 1, username: 'owner', joined_at: '', is_bot: false },
          BOT_PLAYER,
        ],
      },
    }))

    renderRoom()

    await waitFor(() => screen.getByLabelText(/remove bot/i))
    fireEvent.click(screen.getByLabelText(/remove bot/i))

    expect(mockSendMsg).toHaveBeenCalledWith({
      type: 'remove_bot',
      lobby_id: 'lobby-uuid-1',
      bot_uuid: 'bot-uuid-1',
    })
  })
})

// ─── REST leave on disconnected unmount (commit 18ad5b7) ─────────────────────

describe('LobbyRoom — REST leave when WS disconnected', () => {
  test('fetch /lobbies/:id/leave is called on direct unmount even when WS is disconnected', async () => {
    // Before commit 18ad5b7 the cleanup only sent leave_lobby when connected.
    // After the fix it always defers a REST leave regardless of connection state.
    vi.mocked(useWebSocket).mockReturnValue(makeWsReturn({ connected: false }))

    const { unmount } = renderRoom()
    // Wait for findLobbyByCode to resolve and set lobbyId in state.
    await waitFor(() => expect(screen.getByText('← Lobbies')).toBeInTheDocument())
    await act(async () => {})   // flush findLobbyByCode promise

    vi.useFakeTimers()
    try {
      unmount()
      act(() => { vi.advanceTimersByTime(200) })
    } finally {
      vi.useRealTimers()
    }

    // WS leave should NOT have been sent (not connected)
    expect(mockSendMsg).not.toHaveBeenCalledWith(
      expect.objectContaining({ type: 'leave_lobby' })
    )
    // REST leave SHOULD have been called
    const leaveCall = vi.mocked(fetch).mock.calls.find(
      ([url]) => typeof url === 'string' && url.includes('/leave')
    )
    expect(leaveCall).toBeDefined()
    expect(leaveCall?.[1]).toMatchObject({ method: 'POST' })
  })

  test('REST leave is NOT sent when back-nav button was used (leaveSentRef guard)', async () => {
    // handleBack sends WS leave and sets leaveSentRef so the unmount cleanup
    // skips the REST leave — no double-leave. This is intentional design.
    renderRoom()
    await waitFor(() => screen.getByText('← Lobbies'))
    await act(async () => {})

    vi.useFakeTimers()
    try {
      fireEvent.click(screen.getByText('← Lobbies'))
      act(() => { vi.advanceTimersByTime(200) })
    } finally {
      vi.useRealTimers()
    }

    // WS leave was sent synchronously by handleBack
    expect(mockSendMsg).toHaveBeenCalledWith(
      expect.objectContaining({ type: 'leave_lobby' })
    )
    // REST leave should NOT be sent (leaveSentRef prevents it)
    const leaveCall = vi.mocked(fetch).mock.calls.find(
      ([url]) => typeof url === 'string' && url.includes('/leave')
    )
    expect(leaveCall).toBeUndefined()
  })
})

// afterAll: flush any lingering 150ms REST-leave timers scheduled during the
// last test's unmount so they fire before Vitest restores global stubs.
afterAll(async () => {
  await new Promise<void>(r => setTimeout(r, 250))
})

// ─── Bot autofill toggle propagation (lobby_settings_changed bug fix) ────────
// Each test explicitly unmounts with fake timers to flush the 150 ms REST-leave
// cleanup timer before it can leak into the "REST leave is NOT sent" test below.

describe('LobbyRoom — bot autofill toggle propagation', () => {
  test('toggle shows "Off" when lobbyState.spawn_bots_on_leave is false', async () => {
    const { unmount } = renderRoom()
    await waitFor(() => screen.getByText('Bot auto-fill'))
    expect(screen.getByText('Off')).toBeInTheDocument()
    expect(screen.queryByText('On')).toBeNull()
    vi.useFakeTimers()
    try { unmount(); act(() => { vi.advanceTimersByTime(200) }) } finally { vi.useRealTimers() }
  })

  test('toggle shows "On" when lobbyState.spawn_bots_on_leave is true', async () => {
    // Simulate non-creator receiving an updated lobbyState (as if lobby_settings_changed
    // updated it via useWebSocket) — the sync effect sets botAutofill=true.
    vi.mocked(useWebSocket).mockReturnValue(makeWsReturn({
      lobbyState: { ...BASE_LOBBY_STATE, spawn_bots_on_leave: true },
    }))
    const { unmount } = renderRoom()
    await waitFor(() => screen.getByText('Bot auto-fill'))
    expect(screen.getByText('On')).toBeInTheDocument()
    expect(screen.queryByText('Off')).toBeNull()
    vi.useFakeTimers()
    try { unmount(); act(() => { vi.advanceTimersByTime(200) }) } finally { vi.useRealTimers() }
  })

  test('non-owner cannot click the toggle', async () => {
    vi.mocked(useWebSocket).mockReturnValue(makeWsReturn({
      lobbyState: { ...BASE_LOBBY_STATE, creator_id: 99 },  // user.id=1 is not owner
    }))
    const { unmount } = renderRoom()
    await waitFor(() => screen.getByRole('switch', { name: /auto-fill/i }))
    fireEvent.click(screen.getByRole('switch', { name: /auto-fill/i }))
    const patchCall = vi.mocked(fetch).mock.calls.find(
      ([url]) => typeof url === 'string' && url.includes('bot-settings')
    )
    expect(patchCall).toBeUndefined()
    vi.useFakeTimers()
    try { unmount(); act(() => { vi.advanceTimersByTime(200) }) } finally { vi.useRealTimers() }
  })

  test('owner clicking toggle sends PATCH and optimistically updates UI', async () => {
    vi.mocked(fetch).mockImplementation((url) => {
      if (typeof url === 'string' && url.includes('bot-settings')) {
        return Promise.resolve({
          ok: true,
          json: () => Promise.resolve({ spawn_bots_on_leave: true, bot_spawn_difficulty: 'medium' }),
        } as Response)
      }
      return Promise.resolve({
        ok: true,
        json: () => Promise.resolve({ lobbies: [{ id: 'lobby-uuid-1', code: 'ABC123', creator_id: 1, status: 'waiting', min_players: 2, max_players: 8, player_count: 2, created_at: '' }] }),
      } as Response)
    })

    const { unmount } = renderRoom()
    await waitFor(() => screen.getByRole('switch', { name: /auto-fill/i }))
    expect(screen.getByText('Off')).toBeInTheDocument()

    await act(async () => {
      fireEvent.click(screen.getByRole('switch', { name: /auto-fill/i }))
    })

    expect(screen.getByText('On')).toBeInTheDocument()
    const patchCall = vi.mocked(fetch).mock.calls.find(
      ([url]) => typeof url === 'string' && url.includes('bot-settings')
    )
    expect(patchCall).toBeDefined()
    expect(patchCall?.[1]).toMatchObject({ method: 'PATCH' })

    // Flush the 150 ms cleanup timer so it doesn't leak into the REST-leave tests.
    vi.useFakeTimers()
    try { unmount(); act(() => { vi.advanceTimersByTime(200) }) } finally { vi.useRealTimers() }
  })
})

// ─── CSS regression guards ────────────────────────────────────────────────────

import lobbyRoomCss from '../LobbyRoom.css?raw'

describe('LobbyRoom.css — circle guarantees', () => {
  const css = lobbyRoomCss

  test('seat__avatar uses aspect-ratio:1 to stay circular', () => {
    expect(css).toMatch(/\.seat__avatar\s*\{[^}]*aspect-ratio:\s*1/)
    expect(css).not.toMatch(/\.seat__avatar\s*\{[^}]*height:\s*clamp\(/)
  })

  test('seat--empty uses aspect-ratio:1 to stay circular', () => {
    expect(css).toMatch(/\.seat--empty\s*\{[^}]*aspect-ratio:\s*1/)
    expect(css).not.toMatch(/\.seat--empty\s*\{[^}]*height:\s*clamp\(/)
  })
})
