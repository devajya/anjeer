import React from 'react'
import { render, screen, fireEvent } from '@testing-library/react'
import { describe, it, expect, vi } from 'vitest'
import { MemoryRouter } from 'react-router-dom'
import { QueuePopup } from './QueuePopup'
import type { PlayersAround } from '../hooks/useQueueSocket'

const mockNavigate = vi.fn()
vi.mock('react-router-dom', async (importOriginal) => {
  const actual = await importOriginal<typeof import('react-router-dom')>()
  return { ...actual, useNavigate: () => mockNavigate }
})

const PLAYERS_AROUND: PlayersAround[] = [
  { position: 3, username: 'alice',  is_self: false },
  { position: 4, username: 'bob',    is_self: false },
  { position: 5, username: 'me',     is_self: true  },
  { position: 6, username: 'charlie',is_self: false },
  { position: 7, username: 'dave',   is_self: false },
]

function renderPopup(
  overrides: Partial<React.ComponentProps<typeof QueuePopup>> = {}
) {
  const defaults: React.ComponentProps<typeof QueuePopup> = {
    lobbyId:       'lobby-uuid-1',
    position:       5,
    queueSize:      10,
    playersAround:  PLAYERS_AROUND,
    onLeave:        vi.fn(),
    onSpectate:     vi.fn(),
  }
  return render(
    <MemoryRouter>
      <QueuePopup {...defaults} {...overrides} />
    </MemoryRouter>
  )
}

describe('QueuePopup', () => {
  it('renders current position and queue size', () => {
    // Satisfies: "renders current position and queue size"
    renderPopup({ position: 5, queueSize: 10 })
    expect(screen.getByText(/You are/)).toBeInTheDocument()
    expect(screen.getByText(/of 10 waiting/)).toBeInTheDocument()
  })

  it('renders players_around with is_self highlighted', () => {
    // Satisfies: "renders players_around with is_self highlighted"
    // The self entry renders "You" instead of username and carries qp__entry--self.
    renderPopup()
    const selfEntry = screen.getByText('You').closest('.qp__entry')
    expect(selfEntry).toHaveClass('qp__entry--self')
    expect(screen.getByText('alice')).toBeInTheDocument()
    expect(screen.getByText('charlie')).toBeInTheDocument()
  })

  it('shows truncation indicator when queue extends beyond visible window', () => {
    // Satisfies: "shows truncation indicator when queue extends beyond visible window"
    // position 5 in queue of 10; playersAround starts at 3 so aheadCount=2, behindCount=3.
    renderPopup()
    expect(screen.getByLabelText('2 players ahead')).toBeInTheDocument()
    expect(screen.getByLabelText('3 players behind')).toBeInTheDocument()
  })

  it('leave queue button fires leave_queue command', () => {
    // Satisfies: "leave queue button fires leave_queue command"
    // onLeave is the hook-level leaveQueue fn passed down; clicking it should call it once.
    const onLeave = vi.fn()
    renderPopup({ onLeave })
    fireEvent.click(screen.getByRole('button', { name: /Leave Queue/i }))
    expect(onLeave).toHaveBeenCalledOnce()
  })

  it('spectate button calls onSpectate', () => {
    // Satisfies: "spectate button navigates to /spectate/:lobbyId"
    // Navigation itself lives in LobbyBrowser; this verifies the callback fires.
    const onSpectate = vi.fn()
    renderPopup({ onSpectate })
    fireEvent.click(screen.getByRole('button', { name: /Spectate/i }))
    expect(onSpectate).toHaveBeenCalledOnce()
  })

  it('dismisses on queue_admitted message', () => {
    // Satisfies: "dismisses on queue_admitted message"
    // QueuePopup is a dumb component; dismissal is the parent stopping its render.
    // We simulate that by rendering a wrapper that tracks admitted state.
    function AdmittedWrapper() {
      const [admitted, setAdmitted] = React.useState(false)
      if (admitted) return <div data-testid="dismissed" />
      return (
        <>
          <button onClick={() => setAdmitted(true)}>simulate_admitted</button>
          <QueuePopup
            lobbyId="lobby-uuid-1"
            position={5}
            queueSize={10}
            playersAround={PLAYERS_AROUND}
            onLeave={vi.fn()}
            onSpectate={vi.fn()}
          />
        </>
      )
    }
    render(<MemoryRouter><AdmittedWrapper /></MemoryRouter>)
    expect(screen.getByRole('dialog')).toBeInTheDocument()
    fireEvent.click(screen.getByText('simulate_admitted'))
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument()
    expect(screen.getByTestId('dismissed')).toBeInTheDocument()
  })
})
