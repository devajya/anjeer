import { describe, it, expect, vi } from 'vitest'
import { render, screen } from '@testing-library/react'
import { MemoryRouter } from 'react-router-dom'
import { Login } from '../Login'

vi.mock('../../hooks/useAuth', () => ({ useAuth: vi.fn() }))

import { useAuth } from '../../hooks/useAuth'

function setAuth(user: { id: number; username: string; games_played: number } | null, loading = false) {
  vi.mocked(useAuth).mockReturnValue({ user, loading, logout: vi.fn() })
}

describe('Login page — Task 3 ACs', () => {
  it('"Back to home" link has href="/"', () => {
    setAuth(null)
    render(
      <MemoryRouter initialEntries={['/auth']}>
        <Login />
      </MemoryRouter>,
    )
    const link = screen.getByRole('link', { name: /back to home/i })
    expect(link).toBeInTheDocument()
    expect(link).toHaveAttribute('href', '/')
  })

  it('card element carries login__card class (surface background token applied)', () => {
    setAuth(null)
    const { container } = render(
      <MemoryRouter initialEntries={['/auth']}>
        <Login />
      </MemoryRouter>,
    )
    expect(container.querySelector('.login__card')).toBeInTheDocument()
  })

  it('route is /auth — sign-in form visible at /auth', () => {
    setAuth(null)
    render(
      <MemoryRouter initialEntries={['/auth']}>
        <Login />
      </MemoryRouter>,
    )
    expect(screen.getByText(/sign in/i)).toBeInTheDocument()
  })
})
