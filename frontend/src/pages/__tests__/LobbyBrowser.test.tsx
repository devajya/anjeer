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
  owner_id: 2,
  status: 'waiting',
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
          ...LOBBY_ABC, id: 'new-uuid', code: 'NEW999', owner_id: 1, player_count: 1,
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
      expect(screen.getByText(/No open lobbies/i)).toBeInTheDocument()
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
      expect(mockNavigate).toHaveBeenCalledWith('/lobby/NEW999')
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
      expect(mockNavigate).toHaveBeenCalledWith('/lobby/ABC123')
    })
  })

  test('shows error when code not in list', async () => {
    vi.stubGlobal('fetch', makeFetch({ list: [] }))
    renderBrowser()

    await waitFor(() => screen.getByText(/No open lobbies/i))

    fireEvent.change(screen.getByLabelText('Lobby code'), { target: { value: 'XXXXXX' } })
    fireEvent.click(screen.getByText('Join by Code'))

    expect(screen.getByRole('alert')).toHaveTextContent(/Lobby not found/i)
    expect(mockNavigate).not.toHaveBeenCalled()
  })
})
