import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { render, act } from '@testing-library/react'
import { MemoryRouter } from 'react-router-dom'
import { Game } from '../Game'

// ── Module mocks ──────────────────────────────────────────────────────────────

vi.mock('../../hooks/useAuth', () => ({
  useAuth: () => ({ user: { id: 1, username: 'tester' }, loading: false, logout: vi.fn() }),
}))
vi.mock('../../hooks/useKeyBinds', () => ({
  useKeyBinds: () => ({ binds: {}, loading: false }),
}))
vi.mock('../../hooks/useKeyboardShortcuts', () => ({
  useKeyboardShortcuts: () => {},
}))

// Mutable WS defaults — all tests start with a connected, in-game player.
const WS_DEFAULTS = {
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
  playerSlot: null,   // null = slot not yet assigned (the grace-period scenario)
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
}

let wsOverrides: Record<string, unknown> = {}
vi.mock('../../hooks/useWebSocket', () => ({
  useWebSocket: () => ({ ...WS_DEFAULTS, ...wsOverrides }),
}))

// useReconnect is mocked so each test can inject the reconnectStatus it needs.
// onWindowExpired is a spy so we can assert it was / wasn't called.
let mockOnWindowExpired: ReturnType<typeof vi.fn>
let reconnectStatusOverride: string = 'idle'

vi.mock('../../hooks/useReconnect', () => ({
  useReconnect: () => ({
    status: reconnectStatusOverride,
    onTokenReceived: vi.fn(),
    onSnapshotReceived: vi.fn(),
    onWindowExpired: mockOnWindowExpired,
  }),
}))

vi.stubGlobal('fetch', vi.fn().mockResolvedValue({
  ok: true,
  json: async () => ({ reconnect_window_seconds: 20 }),
} as Response))

const lsStore: Record<string, string> = {}
vi.stubGlobal('localStorage', {
  getItem:    (k: string) => lsStore[k] ?? null,
  setItem:    (k: string, v: string) => { lsStore[k] = v },
  removeItem: (k: string) => { delete lsStore[k] },
  clear:      () => { Object.keys(lsStore).forEach(k => delete lsStore[k]) },
})

function renderGame() {
  return render(
    <MemoryRouter initialEntries={['/game?lobby_id=TESTLOB']}>
      <Game />
    </MemoryRouter>,
  )
}

beforeEach(() => {
  wsOverrides = {}
  reconnectStatusOverride = 'idle'
  mockOnWindowExpired = vi.fn()
  vi.clearAllMocks()
  vi.useFakeTimers()
  // Re-stub fetch after clearAllMocks resets it
  vi.mocked(fetch as ReturnType<typeof vi.fn>).mockResolvedValue({
    ok: true,
    json: async () => ({ reconnect_window_seconds: 20 }),
  } as Response)
})

afterEach(() => {
  vi.runAllTimers()
  vi.useRealTimers()
})

// ── Tests ─────────────────────────────────────────────────────────────────────

describe('Game — grace-period guard (commit 18ad5b7 fix)', () => {
  it('does NOT call onWindowExpired after 3s when reconnectStatus is "connected"', async () => {
    // Before the fix, the condition was `reconnectStatus !== 'connected'` which
    // inverted the intent — connected players would trigger the grace-period timer.
    reconnectStatusOverride = 'connected'
    wsOverrides = { connected: true, playerSlot: null }

    renderGame()
    act(() => { vi.advanceTimersByTime(4000) })

    expect(mockOnWindowExpired).not.toHaveBeenCalled()
  })

  it('does NOT call onWindowExpired when playerSlot is assigned (normal gameplay)', async () => {
    reconnectStatusOverride = 'reconnecting'
    wsOverrides = { connected: true, playerSlot: 0 }

    renderGame()
    act(() => { vi.advanceTimersByTime(4000) })

    expect(mockOnWindowExpired).not.toHaveBeenCalled()
  })

  it('DOES call onWindowExpired after 3s when reconnectStatus is "reconnecting" and slot is null', async () => {
    reconnectStatusOverride = 'reconnecting'
    wsOverrides = { connected: true, playerSlot: null }

    renderGame()
    act(() => { vi.advanceTimersByTime(3500) })

    expect(mockOnWindowExpired).toHaveBeenCalledTimes(1)
  })

  it('does NOT call onWindowExpired when disconnected (WS is closed)', async () => {
    reconnectStatusOverride = 'reconnecting'
    wsOverrides = { connected: false, playerSlot: null }

    renderGame()
    act(() => { vi.advanceTimersByTime(4000) })

    expect(mockOnWindowExpired).not.toHaveBeenCalled()
  })
})

// ── noTokenOverflow guard — fresh player joins at game start (bug fix) ─────────
// Regression: a player who connects to /game while the server is in Countdown
// phase would miss round_starting. Before the server fix, isInGame stayed false
// and after 3 s the StaleLobbyModal ("This lobby is full") appeared. These tests
// document the client-side invariants: the timer is suppressed whenever any
// isInGame signal is present.

describe('Game — noTokenOverflow timer (fresh player at game start)', () => {
  it('does NOT show StaleLobbyModal when startsAt is set (round_starting received)', async () => {
    // startsAt set means the client received round_starting — isInGame=true.
    reconnectStatusOverride = 'connected'
    wsOverrides = {
      connected: true,
      playerSlot: null,
      startsAt: '2099-01-01T00:00:05.000Z',
    }

    const { queryByRole } = renderGame()
    act(() => { vi.advanceTimersByTime(4000) })

    expect(queryByRole('dialog')).toBeNull()
  })

  it('does NOT show StaleLobbyModal when waitingForStart is set', async () => {
    reconnectStatusOverride = 'connected'
    wsOverrides = {
      connected: true,
      playerSlot: null,
      waitingForStart: { type: 'waiting_for_start', connected: 1, required: 2 },
    }

    const { queryByRole } = renderGame()
    act(() => { vi.advanceTimersByTime(4000) })

    expect(queryByRole('dialog')).toBeNull()
  })

  it('shows StaleLobbyModal after 3s when connected but no game state arrives', async () => {
    // All isInGame signals absent → noTokenOverflow fires → StaleLobbyModal shown.
    reconnectStatusOverride = 'connected'
    wsOverrides = {
      connected: true,
      playerSlot: null,
      startsAt: null,
      waitingForStart: null,
      hand: null,
    }

    const { queryByRole } = renderGame()
    act(() => { vi.advanceTimersByTime(3100) })

    expect(queryByRole('dialog')).not.toBeNull()
  })
})
