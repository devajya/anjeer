import { render, screen, fireEvent } from '@testing-library/react'
import { describe, it, expect, vi } from 'vitest'
import { PlayerBadge } from './PlayerBadge'

// ---------------------------------------------------------------------------
// Regression: Game.tsx must send leave_lobby before navigating when the user
// clicks Leave.  PlayerBadge is the boundary component — it calls onLeave,
// which Game.tsx wires to sendMessage({ type: 'leave_lobby', ... }) followed
// by navigate().  If onLeave stops firing (e.g. onClick removed or button
// replaced), the leave_lobby message is never sent, the WS closes on unmount,
// and the server starts a reconnect window instead of processing a permanent
// leave.  See G-T3c in game_session_tests.cpp for the server-side contract.
// ---------------------------------------------------------------------------

describe('PlayerBadge', () => {
  it('renders the player username', () => {
    render(<PlayerBadge username="alice" onLeave={vi.fn()} />)
    expect(screen.getByText('alice')).toBeInTheDocument()
  })

  it('calls onLeave when the Leave button is clicked', () => {
    const onLeave = vi.fn()
    render(<PlayerBadge username="alice" onLeave={onLeave} />)
    fireEvent.click(screen.getByRole('button', { name: /leave game/i }))
    expect(onLeave).toHaveBeenCalledOnce()
  })
})
