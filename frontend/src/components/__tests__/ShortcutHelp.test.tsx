import { render, screen, fireEvent } from '@testing-library/react'
import { describe, test, expect, vi } from 'vitest'
import { ShortcutHelp } from '../ShortcutHelp'
import { DEFAULT_BINDS } from '../../hooks/useKeyBinds'

describe('ShortcutHelp — renders', () => {
  test('renders all 11 action labels', () => {
    render(<ShortcutHelp binds={DEFAULT_BINDS} onClose={vi.fn()} />)
    expect(screen.getByText('Focus Clubs')).toBeInTheDocument()
    expect(screen.getByText('Focus Diamonds')).toBeInTheDocument()
    expect(screen.getByText('Focus Hearts')).toBeInTheDocument()
    expect(screen.getByText('Focus Spades')).toBeInTheDocument()
    expect(screen.getByText('Submit Buy')).toBeInTheDocument()
    expect(screen.getByText('Submit Sell')).toBeInTheDocument()
    expect(screen.getByText('Nudge Buy +1')).toBeInTheDocument()
    expect(screen.getByText('Nudge Sell −1')).toBeInTheDocument()
    expect(screen.getByText('Cancel Best Buy')).toBeInTheDocument()
    expect(screen.getByText('Cancel Best Sell')).toBeInTheDocument()
    expect(screen.getByText('Toggle This Panel')).toBeInTheDocument()
  })

  test('renders default key combos as kbd elements', () => {
    render(<ShortcutHelp binds={DEFAULT_BINDS} onClose={vi.fn()} />)
    expect(screen.getByText('c')).toBeInTheDocument()
    expect(screen.getByText('Control+z')).toBeInTheDocument()
  })

  test('shows custom bind when provided', () => {
    render(<ShortcutHelp binds={{ ...DEFAULT_BINDS, suit_clubs: 'q' }} onClose={vi.fn()} />)
    expect(screen.getByText('q')).toBeInTheDocument()
  })

  test('shows — when action has no bind', () => {
    const binds: Record<string, string> = { ...DEFAULT_BINDS }
    delete binds['suit_clubs']
    render(<ShortcutHelp binds={binds} onClose={vi.fn()} />)
    expect(screen.getByText('—')).toBeInTheDocument()
  })
})

describe('ShortcutHelp — close behaviour', () => {
  test('calls onClose when close button is clicked', () => {
    const onClose = vi.fn()
    render(<ShortcutHelp binds={DEFAULT_BINDS} onClose={onClose} />)
    fireEvent.click(screen.getByLabelText('Close shortcuts panel'))
    expect(onClose).toHaveBeenCalledOnce()
  })

  test('calls onClose when backdrop is clicked', () => {
    const onClose = vi.fn()
    const { container } = render(<ShortcutHelp binds={DEFAULT_BINDS} onClose={onClose} />)
    fireEvent.click(container.querySelector('.shortcut-help__backdrop')!)
    expect(onClose).toHaveBeenCalledOnce()
  })

  test('calls onClose when Escape is pressed', () => {
    const onClose = vi.fn()
    render(<ShortcutHelp binds={DEFAULT_BINDS} onClose={onClose} />)
    fireEvent.keyDown(window, { key: 'Escape' })
    expect(onClose).toHaveBeenCalledOnce()
  })

  test('does not call onClose when panel interior is clicked', () => {
    const onClose = vi.fn()
    const { container } = render(<ShortcutHelp binds={DEFAULT_BINDS} onClose={onClose} />)
    fireEvent.click(container.querySelector('.shortcut-help__panel')!)
    expect(onClose).not.toHaveBeenCalled()
  })
})
