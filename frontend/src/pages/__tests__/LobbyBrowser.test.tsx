import { render, screen, fireEvent, waitFor } from '@testing-library/react'
import { describe, test, expect, vi, beforeEach, afterEach } from 'vitest'
import { MemoryRouter } from 'react-router-dom'
import { LobbyBrowser } from '../LobbyBrowser'
import type { LobbyView } from '../../types/lobby'

// AGENT-CTX: useNavigate is mocked to capture navigation calls. useAuth is
// mocked because LobbyBrowser now checks user.id to disable Join on own lobbies.

const mockNavigate = vi.fn()

vi.mock('react-router-dom', async (importOriginal) => {
  const actual = await importOriginal<typeof import('react-router-dom')>()
  return { ...actual, useNavigate: () => mockNavigate }
})

vi.mock('../../hooks/useAuth', () => ({
  useAuth: () => ({ user: { id: 1, username: 'testuser', games_played: 0 }, logout: vi.fn() })
}))

const LOBBY_ABC: LobbyView = {
  id: 'lobby-uuid-1',
  code: 'ABC123',
  creator_id: 2,
  status: 'waiting',
  mode: 'ui',
  min_players: 2,
  max_players: 8,
  player_count: 1,
  created_at: '2026-04-23T00:00:00Z',
}

function makeFetch(overrides: {
  list?: LobbyView[]
  createResult?: LobbyView
  joinOk?: boolean
  joinError?: string
} = {}) {
  return vi.fn().mockImplementation((url: string, opts?: RequestInit) => {
    const method = opts?.method?.toUpperCase() ?? 'GET'

    if (url === '/lobbies' && method === 'GET') {
      return Promise.resolve({
        ok: true,
        json: () => Promise.resolve({ lobbies: overrides.list ?? [LOBBY_ABC] }),
      })
    }
    if (url === '/lobbies' && method === 'POST') {
      return Promise.resolve({
        ok: true,
        json: () => Promise.resolve(overrides.createResult ?? {
          ...LOBBY_ABC, id: 'new-uuid', code: 'NEW999', creator_id: 1, player_count: 1,
        }),
      })
    }
    if (typeof url === 'string' && url.includes('/join') && method === 'POST') {
      if (overrides.joinError) {
        return Promise.resolve({
          ok: false,
          json: () => Promise.resolve({ error: overrides.joinError }),
        })
      }
      return Promise.resolve({
        ok: true,
        json: () => Promise.resolve({ lobby_id: LOBBY_ABC.id, code: LOBBY_ABC.code }),
      })
    }
    return Promise.resolve({ ok: false, json: () => Promise.resolve({}) })
  })
}

function renderBrowser() {
  return render(
    <MemoryRouter>
      <LobbyBrowser />
    </MemoryRouter>
  )
}

beforeEach(() => {
  vi.clearAllMocks()
})

afterEach(() => {
  vi.restoreAllMocks()
})

// ─── AC2: list open lobbies ───────────────────────────────────────────────────

describe('LobbyBrowser — lobby list', () => {
  test('renders list of waiting lobbies from GET /lobbies', async () => {
    vi.stubGlobal('fetch', makeFetch())
    renderBrowser()

    await waitFor(() => {
      expect(screen.getByText('ABC123')).toBeInTheDocument()
    })
  })

  test('shows "No open lobbies" when list is empty', async () => {
    vi.stubGlobal('fetch', makeFetch({ list: [] }))
    renderBrowser()

    await waitFor(() => {
      expect(screen.getByText(/No games open/i)).toBeInTheDocument()
    })
  })
})

// ─── Create lobby ─────────────────────────────────────────────────────────────

describe('LobbyBrowser — create lobby', () => {
  test('create lobby button calls POST /lobbies and navigates to /lobby/:code', async () => {
    vi.stubGlobal('fetch', makeFetch({ createResult: { ...LOBBY_ABC, code: 'NEW999' } }))
    renderBrowser()

    // Wait for initial list load
    await waitFor(() => screen.getByText('ABC123'))

    fireEvent.click(screen.getByText('Create Lobby'))

    await waitFor(() => {
      expect(mockNavigate).toHaveBeenCalledWith('/lobby/NEW999', expect.objectContaining({ state: expect.objectContaining({ lobbyId: expect.any(String) }) }))
    })
  })
})

// ─── Join by code ─────────────────────────────────────────────────────────────

describe('LobbyBrowser — join by code', () => {
  test('join by code calls POST /lobbies/:id/join and navigates', async () => {
    vi.stubGlobal('fetch', makeFetch())
    renderBrowser()

    await waitFor(() => screen.getByText('ABC123'))

    fireEvent.change(screen.getByLabelText('Lobby code'), { target: { value: 'ABC123' } })
    fireEvent.click(screen.getByText('Join by Code'))

    await waitFor(() => {
      expect(mockNavigate).toHaveBeenCalledWith('/lobby/ABC123', expect.objectContaining({ state: expect.objectContaining({ lobbyId: expect.any(String) }) }))
    })
  })

  test('shows error when code not in list', async () => {
    vi.stubGlobal('fetch', makeFetch({ list: [] }))
    renderBrowser()

    await waitFor(() => screen.getByText(/No games open/i))

    fireEvent.change(screen.getByLabelText('Lobby code'), { target: { value: 'XXXXXX' } })
    fireEvent.click(screen.getByText('Join by Code'))

    expect(screen.getByRole('alert')).toHaveTextContent(/Lobby not found/i)
    expect(mockNavigate).not.toHaveBeenCalled()
  })
})

// ─── Task 9: Active tab ───────────────────────────────────────────────────────

describe('LobbyBrowser — Active tab', () => {
  const ACTIVE_LOBBY: LobbyView = {
    ...LOBBY_ABC,
    id: 'lobby-uuid-2',
    code: 'XYZ999',
    status: 'in_game',
    player_count: 4,
  }

  test('Active tab shows in-progress lobbies', async () => {
    vi.stubGlobal('fetch', makeFetch({ list: [LOBBY_ABC, ACTIVE_LOBBY] }))
    renderBrowser()

    await waitFor(() => screen.getByText('ABC123'))

    fireEvent.click(screen.getByText(/^Active/))

    await waitFor(() => {
      expect(screen.getByText(/XYZ999/)).toBeInTheDocument()
    })
  })

  test('Active tab shows empty state when no games in progress', async () => {
    vi.stubGlobal('fetch', makeFetch({ list: [LOBBY_ABC] }))
    renderBrowser()

    await waitFor(() => screen.getByText('ABC123'))

    fireEvent.click(screen.getByText(/^Active/))

    await waitFor(() => {
      expect(screen.getByText(/No active games/i)).toBeInTheDocument()
    })
  })

  // ── Task 11 additions ──────────────────────────────────────────────────────

  test('Active tab — API in_game lobby shows API mode badge', async () => {
    const apiActive: LobbyView = { ...LOBBY_ABC, id: 'api-active-1', code: 'APIACT', status: 'in_game', mode: 'api' }
    vi.stubGlobal('fetch', makeFetch({ list: [apiActive] }))
    renderBrowser()
    fireEvent.click(screen.getByText(/^Active/))
    await waitFor(() => expect(screen.getByText('API')).toBeInTheDocument())
  })

  test('active lobby row shows Spectate button', async () => {
    const activeLobby: LobbyView = { ...LOBBY_ABC, id: 'active-1', status: 'in_game' }
    vi.stubGlobal('fetch', makeFetch({ list: [activeLobby] }))
    renderBrowser()
    fireEvent.click(screen.getByText(/^Active/))
    await waitFor(() =>
      expect(screen.getByRole('button', { name: /spectate/i })).toBeInTheDocument()
    )
  })

  test('waiting lobby row has no Spectate button', async () => {
    vi.stubGlobal('fetch', makeFetch({ list: [LOBBY_ABC] }))
    renderBrowser()
    await waitFor(() => screen.getByText('ABC123'))
    expect(screen.queryByRole('button', { name: /spectate/i })).not.toBeInTheDocument()
  })

  // Task 9 stub — create form has NO mode toggle; browser always creates UI lobbies
  test('create lobby form has no UI/API mode toggle', async () => {
    vi.stubGlobal('fetch', makeFetch())
    renderBrowser()
    await waitFor(() => screen.getByText('ABC123'))
    expect(screen.queryByRole('button', { name: /^UI$/i })).not.toBeInTheDocument()
    expect(screen.queryByRole('button', { name: /^API$/i })).not.toBeInTheDocument()
  })
})

// ─── Task 9 stubs — mode-filtered Starting tab (RED before Task 9 impl) ──────

describe('LobbyBrowser — mode separation (Task 9)', () => {
  const API_WAITING: LobbyView = { ...LOBBY_ABC, id: 'api-uuid-1', code: 'APIABC', mode: 'api' }
  const UI_WAITING: LobbyView  = { ...LOBBY_ABC, id: 'ui-uuid-1',  code: 'UIABC',  mode: 'ui'  }

  test('Starting tab shows only UI-mode waiting lobbies', async () => {
    vi.stubGlobal('fetch', makeFetch({ list: [UI_WAITING, API_WAITING] }))
    renderBrowser()
    await waitFor(() => screen.getByText('UIABC'))
    expect(screen.queryByText('APIABC')).not.toBeInTheDocument()
  })

  test('Starting tab does not show API-mode waiting lobbies', async () => {
    vi.stubGlobal('fetch', makeFetch({ list: [API_WAITING] }))
    renderBrowser()
    await waitFor(() => screen.getByText(/No games open/i))
    expect(screen.queryByText('APIABC')).not.toBeInTheDocument()
  })
})
