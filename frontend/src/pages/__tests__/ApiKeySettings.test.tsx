import { render, screen, waitFor, fireEvent } from '@testing-library/react'
import { describe, test, expect, vi, beforeEach } from 'vitest'
import { MemoryRouter } from 'react-router-dom'
import { ApiKeySettings } from '../ApiKeySettings'

vi.mock('react-router-dom', async (importOriginal) => {
  const actual = await importOriginal<typeof import('react-router-dom')>()
  return { ...actual, useNavigate: () => vi.fn() }
})

vi.mock('../../context/AuthContext', () => ({
  useAuth: () => ({ user: { id: 1, username: 'testuser' }, loading: false }),
}))

function renderPage() {
  return render(
    <MemoryRouter>
      <ApiKeySettings />
    </MemoryRouter>
  )
}

beforeEach(() => {
  vi.stubGlobal('fetch', vi.fn())
})

describe('ApiKeySettings', () => {
  test('renders empty state when no keys exist', async () => {
    vi.mocked(fetch).mockResolvedValueOnce({
      ok: true, json: async () => [],
    } as Response)

    renderPage()
    await waitFor(() =>
      expect(screen.getByText(/no keys yet/i)).toBeInTheDocument()
    )
  })

  test('Generate button opens modal with key text', async () => {
    vi.mocked(fetch)
      .mockResolvedValueOnce({ ok: true, json: async () => [] } as Response)
      .mockResolvedValueOnce({
        ok: true,
        status: 201,
        json: async () => ({
          id: 1,
          key: 'ank_' + 'a'.repeat(64),
          name: 'my-bot',
          expires_at: new Date(Date.now() + 86400000 * 30).toISOString(),
        }),
      } as Response)

    renderPage()
    await waitFor(() => screen.getByPlaceholderText(/key name/i))

    fireEvent.change(screen.getByPlaceholderText(/key name/i), {
      target: { value: 'my-bot' },
    })
    fireEvent.click(screen.getByRole('button', { name: /^generate$/i }))

    await waitFor(() =>
      expect(screen.getByText(/ank_/)).toBeInTheDocument()
    )
  })

  test('modal key text is not visible after close', async () => {
    vi.mocked(fetch)
      .mockResolvedValueOnce({ ok: true, json: async () => [] } as Response)
      .mockResolvedValueOnce({
        ok: true,
        status: 201,
        json: async () => ({
          id: 1,
          key: 'ank_' + 'b'.repeat(64),
          name: 'bot2',
          expires_at: new Date(Date.now() + 86400000 * 30).toISOString(),
        }),
      } as Response)

    renderPage()
    await waitFor(() => screen.getByPlaceholderText(/key name/i))

    fireEvent.change(screen.getByPlaceholderText(/key name/i), {
      target: { value: 'bot2' },
    })
    fireEvent.click(screen.getByRole('button', { name: /^generate$/i }))
    await waitFor(() => screen.getByText(/done/i))

    fireEvent.click(screen.getByRole('button', { name: /done/i }))
    await waitFor(() =>
      expect(screen.queryByText(/ank_/)).not.toBeInTheDocument()
    )
  })

  test('Revoke calls DELETE /players/me/api-keys/:id', async () => {
    const futureExpiry = new Date(Date.now() + 86400000 * 30).toISOString()
    vi.mocked(fetch)
      .mockResolvedValueOnce({
        ok: true,
        json: async () => ([{
          id: 42, name: 'my-bot', created_at: new Date().toISOString(),
          expires_at: futureExpiry, revoked_at: null,
        }]),
      } as Response)
      .mockResolvedValueOnce({ ok: true, json: async () => ({}) } as Response)

    renderPage()
    await waitFor(() => screen.getByText('my-bot'))

    fireEvent.click(screen.getByRole('button', { name: /revoke/i }))

    await waitFor(() =>
      expect(vi.mocked(fetch)).toHaveBeenCalledWith(
        '/players/me/api-keys/42',
        expect.objectContaining({ method: 'DELETE' })
      )
    )
  })

  test('existing active key disables Generate button', async () => {
    const futureExpiry = new Date(Date.now() + 86400000 * 30).toISOString()
    vi.mocked(fetch).mockResolvedValueOnce({
      ok: true,
      json: async () => ([{
        id: 1, name: 'active-bot', created_at: new Date().toISOString(),
        expires_at: futureExpiry, revoked_at: null,
      }]),
    } as Response)

    renderPage()
    await waitFor(() => screen.getByText('active-bot'))

    expect(screen.getByRole('button', { name: /^generate$/i })).toBeDisabled()
  })
})
