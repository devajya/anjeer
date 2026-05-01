import { render, screen, fireEvent } from '@testing-library/react'
import { describe, test, expect, vi } from 'vitest'
import { MemoryRouter } from 'react-router-dom'
import { SessionError } from '../SessionError'
import type { SessionErrorMessage } from '../../types/messages'

const mockNavigate = vi.fn()

vi.mock('react-router-dom', async (importOriginal) => {
  const actual = await importOriginal<typeof import('react-router-dom')>()
  return { ...actual, useNavigate: () => mockNavigate }
})

const SESSION_ERROR: SessionErrorMessage = {
  type: 'session_error',
  message: 'An internal error terminated the session.',
}

function renderError(msg = SESSION_ERROR) {
  return render(
    <MemoryRouter>
      <SessionError sessionError={msg} />
    </MemoryRouter>
  )
}

// ── F3: SessionError renders with lobby button ────────────────────────────────

describe('SessionError renders with lobby button', () => {
  test('renders "Session Error" heading', () => {
    renderError()
    expect(screen.getByRole('heading', { name: /Session Error/i })).toBeInTheDocument()
  })

  test('displays the server error message', () => {
    renderError()
    expect(screen.getByText(SESSION_ERROR.message)).toBeInTheDocument()
  })

  test('Return to Lobby button navigates to /lobby', () => {
    renderError()
    fireEvent.click(screen.getByRole('button', { name: /Return to Lobby/i }))
    expect(mockNavigate).toBeCalledWith('/lobby')
  })

  test('has alert role for immediate screen-reader announcement', () => {
    renderError()
    expect(screen.getByRole('alert')).toBeInTheDocument()
  })
})
