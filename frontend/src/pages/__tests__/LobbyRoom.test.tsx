import { render, screen, waitFor } from '@testing-library/react'
import { describe, test, expect, vi, beforeEach } from 'vitest'
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
  owner_id: 1,            // matches user.id=1 → this user is the owner
  status: 'waiting',
  min_players: 2,
  max_players: 8,
  players: [
    { player_id: 1, username: 'owner', joined_at: '2026-04-23T00:00:00Z' },
    { player_id: 2, username: 'alice', joined_at: '2026-04-23T00:00:00Z' },
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
    sendMessage:       mockSendMsg,
    subscribeLobby:    mockSubscribe,
    unsubscribeLobby:  mockUnsub,
    ownsBestBidBySuit: {},
    ownsBestAskBySuit: {},
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
        owner_id:     1,
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

// ─── AC4: Start button visibility ────────────────────────────────────────────

describe('LobbyRoom — start button', () => {
  test('start button visible only for lobby owner with enough players', async () => {
    // owner_id=1 matches user.id=1; 2 players ≥ min_players=2
    renderRoom()

    await waitFor(() => {
      expect(screen.getByText('Start Game')).toBeInTheDocument()
    })
  })

  test('start button hidden for non-owner', async () => {
    // owner_id=99 — does not match user.id=1
    vi.mocked(useWebSocket).mockReturnValue(makeWsReturn({
      lobbyState: { ...BASE_LOBBY_STATE, owner_id: 99 },
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
        players: [{ player_id: 1, username: 'owner', joined_at: '' }],
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
          { player_id: 1, username: 'owner', joined_at: '' },
          { player_id: 2, username: 'alice', joined_at: '' },
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
        players: [{ player_id: 1, username: 'owner', joined_at: '' }],
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
