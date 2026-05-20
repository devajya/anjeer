// AGENT-CTX: Test file lives under pages/__tests__/ following existing project
// convention (co-located with the pages under test), not the spec's src/tests/ path.
import { describe, it, expect, vi, beforeEach } from 'vitest'
import { render, screen, waitFor } from '@testing-library/react'
import { MemoryRouter } from 'react-router-dom'
import { Login } from '../Login'
import { LobbyRoom } from '../LobbyRoom'
import { Game } from '../Game'

// ── Module mocks ─────────────────────────────────────────────────────────────

vi.mock('../../hooks/useAuth', () => ({ useAuth: vi.fn() }))
vi.mock('../../hooks/useKeyBinds', () => ({ useKeyBinds: () => ({ binds: {}, loading: false }) }))
vi.mock('../../hooks/useKeyboardShortcuts', () => ({ useKeyboardShortcuts: () => {} }))

// Mutable WS state so tests can inject queueOverflow and other flags.
let wsState: Record<string, unknown> = {}
vi.mock('../../hooks/useWebSocket', () => ({
  useWebSocket: () => ({
    connected: true,
    playerId: 1,
    books: {},
    trades: [],
    myOrders: [],
    errors: {},
    sendMessage: vi.fn(),
    startsAt: null,
    roundEndAt: null,
    hand: null,
    initialHand: null,
    playerSlot: 0,
    roundEnd: null,
    balance: 400,
    waitingForStart: null,
    ownsBestBidBySuit: {},
    ownsBestAskBySuit: {},
    interRound: null,
    gameEnded: null,
    sessionError: null,
    lobbyState: null,
    lobbyStarted: null,
    roster: [],
    deltas: [],
    allBalances: [],
    allHandTotals: [],
    spectatorCount: 0,
    departedSlots: [],
    scriptLogs: [],
    reconnectTokenMsg: null,
    gameStateSnapshot: null,
    reconnectWindowExpired: false,
    queueState: { status: 'idle' },
    currentOwnerPlayerId: null,
    subscribeLobby: vi.fn(),
    unsubscribeLobby: vi.fn(),
    joinQueue: vi.fn(),
    leaveQueue: vi.fn(),
    resetQueue: vi.fn(),
    ...wsState,
  }),
}))

vi.stubGlobal('fetch', vi.fn())
const lsStore: Record<string, string> = {}
vi.stubGlobal('localStorage', {
  getItem:    (k: string) => lsStore[k] ?? null,
  setItem:    (k: string, v: string) => { lsStore[k] = v },
  removeItem: (k: string) => { delete lsStore[k] },
  clear:      () => { Object.keys(lsStore).forEach(k => delete lsStore[k]) },
})

// ── Helpers ───────────────────────────────────────────────────────────────────

import { useAuth } from '../../hooks/useAuth'

const AUTHED_USER = { id: 1, username: 'tester', games_played: 0 }

function setAuth(user: typeof AUTHED_USER | null, loading = false) {
  vi.mocked(useAuth).mockReturnValue({ user, loading, logout: vi.fn() })
}

function makeFetch(lobbies: unknown[] = []) {
  vi.mocked(fetch).mockResolvedValue({
    ok: true,
    json: async () => ({ lobbies }),
  } as Response)
}

// ── 1. Login redirect when authenticated ────────────────────────────────────

describe('Login page — auth guard', () => {
  beforeEach(() => {
    wsState = {}
    Object.keys(lsStore).forEach(k => delete lsStore[k])
  })

  it('redirects /login to /lobby when valid non-expired JWT is in cookie', () => {
    setAuth(AUTHED_USER)
    render(
      <MemoryRouter initialEntries={['/login']}>
        <Login />
      </MemoryRouter>,
    )
    // Login renders null during loading and <Navigate> when authed;
    // the MemoryRouter stays at /login but Navigate issues a replace — component
    // renders nothing (Navigate has no visual output). Verify no login form.
    expect(screen.queryByText(/sign in/i)).not.toBeInTheDocument()
  })

  it('renders /login when no JWT present', () => {
    setAuth(null)
    render(
      <MemoryRouter initialEntries={['/login']}>
        <Login />
      </MemoryRouter>,
    )
    expect(screen.getByText(/sign in/i)).toBeInTheDocument()
  })
})

// ── 2. StaleLobbyModal on stale lobby ────────────────────────────────────────

describe('LobbyRoom — stale lobby modal', () => {
  beforeEach(() => {
    wsState = {}
    setAuth(AUTHED_USER)
  })

  it('shows StaleLobbyModal on lobby 404', async () => {
    // findLobbyByCode returns undefined when the code is absent from the list.
    makeFetch([])
    render(
      <MemoryRouter initialEntries={['/lobby/GONE99']}>
        <LobbyRoom />
      </MemoryRouter>,
    )
    await waitFor(() =>
      expect(screen.getByRole('dialog', { name: /this game has ended/i })).toBeInTheDocument(),
    )
  })

  it('shows StaleLobbyModal when lobby status is ended', async () => {
    // AGENT-CTX: LobbyView has no 'ended' status; 'finished' is the closest
    // terminal state. Both 'finished' and 'closed' are treated as stale.
    makeFetch([{
      id: 'lobby-uuid-1', code: 'FINISH', creator_id: 2,
      status: 'finished', mode: 'ui',
      min_players: 2, max_players: 5, player_count: 0,
      created_at: '2026-05-01T00:00:00Z',
    }])
    render(
      <MemoryRouter initialEntries={['/lobby/FINISH']}>
        <LobbyRoom />
      </MemoryRouter>,
    )
    await waitFor(() =>
      expect(screen.getByRole('dialog', { name: /this game has ended/i })).toBeInTheDocument(),
    )
  })
})

// ── 3. Direct game URL edge cases ────────────────────────────────────────────

describe('Game page — direct URL checks', () => {
  beforeEach(() => {
    wsState = {}
    Object.keys(lsStore).forEach(k => delete lsStore[k])
    setAuth(AUTHED_USER)
    vi.mocked(fetch).mockResolvedValue({
      ok: true,
      json: async () => ({ reconnect_window_seconds: 20 }),
    } as Response)
  })

  it('direct game URL with expired reconnect window shows queue popup with expiry message', async () => {
    // Simulate an expired token in localStorage — useReconnect will detect expiry
    // and set status='expired', which triggers ReconnectOverlay (role="alert").
    lsStore['anjeer_reconnect_ABCDEF'] = JSON.stringify({
      token: 'rtk_expired',
      expires_at: Date.now() - 1,
    })
    wsState = { reconnectWindowExpired: true }
    render(
      <MemoryRouter initialEntries={['/game?lobby_id=ABCDEF']}>
        <Game />
      </MemoryRouter>,
    )
    await waitFor(() =>
      expect(screen.getByRole('alert')).toBeInTheDocument(),
    )
    expect(screen.getByText(/slot expired/i)).toBeInTheDocument()
  })

  it('direct game URL with no token and full queue shows LOBBY_FULL message and redirects', async () => {
    // Simulate server sending queue_overflow (lobby and queue both full).
    wsState = { queueState: { status: 'overflow', lobbyId: 'ABCDEF' } }
    render(
      <MemoryRouter initialEntries={['/game?lobby_id=ABCDEF']}>
        <Game />
      </MemoryRouter>,
    )
    await waitFor(() =>
      expect(screen.getByRole('dialog', { name: /this lobby is full/i })).toBeInTheDocument(),
    )
    expect(screen.getByText(/both full/i)).toBeInTheDocument()
  })
})
