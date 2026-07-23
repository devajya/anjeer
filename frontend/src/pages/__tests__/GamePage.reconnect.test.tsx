import { describe, it, expect, vi, beforeEach } from 'vitest'
import { render, screen, act, waitFor } from '@testing-library/react'
import { MemoryRouter } from 'react-router-dom'
import { Game } from '../Game'

// ── Mocks ────────────────────────────────────────────────────────────────────

vi.mock('../../hooks/useAuth', () => ({
  useAuth: () => ({ user: { id: 1, username: 'tester' }, loading: false, logout: vi.fn() }),
}))

vi.mock('../../hooks/useKeyBinds', () => ({
  useKeyBinds: () => ({ binds: {}, loading: false }),
}))

vi.mock('../../hooks/useKeyboardShortcuts', () => ({
  useKeyboardShortcuts: () => {},
}))

// Mutable WS state that tests can override between renders.
let wsState: Partial<ReturnType<typeof import('../../hooks/useWebSocket').useWebSocket>> = {}

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
    roster: [],
    deltas: [],
    allBalances: [],
    allHandTotals: [],
    spectatorCount: 0,
    subscribeLobby: vi.fn(),
    unsubscribeLobby: vi.fn(),
    joinQueue: vi.fn(),
    leaveQueue: vi.fn(),
    resetQueue: vi.fn(),
    scriptLogs: [],
    reconnectTokenMsg: null,
    gameStateSnapshot: null,
    reconnectWindowExpired: false,
    queueState: { status: 'idle' },
    currentOwnerPlayerId: null,
    currentOwnerUsername: '',
    departedSlots: [],
    lobbyState: null,
    lobbyStarted: null,
    mboLogs: {},
    liveOrders: {},
    bookDepths: {},
    gameMode: null,
    ...wsState,
  }),
}))

vi.stubGlobal('fetch', vi.fn())

// localStorage stub
const lsStore: Record<string, string> = {}
vi.stubGlobal('localStorage', {
  getItem:    (k: string) => lsStore[k] ?? null,
  setItem:    (k: string, v: string) => { lsStore[k] = v },
  removeItem: (k: string) => { delete lsStore[k] },
  clear:      () => { Object.keys(lsStore).forEach(k => delete lsStore[k]) },
})

function renderGame(search = '?lobby_id=ABCDEF') {
  return render(
    <MemoryRouter initialEntries={[`/game${search}`]}>
      <Game />
    </MemoryRouter>,
  )
}

beforeEach(() => {
  wsState = {}
  Object.keys(lsStore).forEach(k => delete lsStore[k])
  vi.mocked(fetch).mockResolvedValue({ ok: true, json: async () => ({ reconnect_window_seconds: 20 }) } as Response)
})

// ── Tests ────────────────────────────────────────────────────────────────────

describe('GamePage reconnect integration', () => {
  it('shows ReconnectOverlay when slot has expired', async () => {
    // Expired token: stored but window already closed
    lsStore['anjeer_reconnect_ABCDEF'] = JSON.stringify({ token: 'rtk_seed', expires_at: Date.now() - 1 })
    wsState = { reconnectWindowExpired: true }

    renderGame()
    await waitFor(() => expect(screen.getByRole('alert')).toBeInTheDocument())
    expect(screen.getByText(/slot expired/i)).toBeInTheDocument()
  })

  it('overlay is absent during normal gameplay (game_state_snapshot arrives, no expiry)', async () => {
    // No expired token, snapshot arrives normally — overlay must not appear.
    wsState = {
      connected: true,
      gameStateSnapshot: {
        type: 'game_state_snapshot',
        player_slot: 0,
        hand: { clubs: 3, diamonds: 2, hearts: 1, spades: 2 },
        order_books: {},
        deltas: [],
        round_timer_remaining: 45,
        all_balances: [400],
        all_hand_totals: [8],
        all_scores: [0],
        roster: [{ player_slot: 0, username: 'tester' }],
        reconnect_token: 'rtk_new',
        reconnect_expires_at: Date.now() + 20_000,
      },
    }
    renderGame()
    // Give effects time to run — overlay must not appear
    await waitFor(() => expect(screen.queryByRole('alert')).not.toBeInTheDocument())
  })

  it('token written to localStorage during session persists after unmount', async () => {
    wsState = { connected: true }
    const { unmount } = renderGame()

    act(() => {
      lsStore['anjeer_reconnect_ABCDEF'] = JSON.stringify({
        token: 'rtk_live',
        expires_at: Date.now() + 20_000,
      })
    })

    unmount()
    expect(lsStore['anjeer_reconnect_ABCDEF']).toBeDefined()
  })

  it('expired slot: overlay shown on mount when reconnect_window_expired signal arrives', async () => {
    wsState = { connected: true, reconnectWindowExpired: true }

    renderGame()
    await waitFor(() => expect(screen.getByRole('alert')).toBeInTheDocument())
    expect(screen.getByText(/slot expired/i)).toBeInTheDocument()
  })
})
