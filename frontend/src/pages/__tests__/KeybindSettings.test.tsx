import { describe, it, expect, vi, beforeEach } from 'vitest'
import { render, screen, fireEvent, waitFor } from '@testing-library/react'
import { MemoryRouter } from 'react-router-dom'
import { KeybindSettings } from '../KeybindSettings'
import * as useKeyBindsModule from '../../hooks/useKeyBinds'

const mockUpdate = vi.fn()

const DEFAULT_BINDS: Record<string, string> = {
  suit_clubs:       'c',
  suit_diamonds:    'd',
  suit_hearts:      'h',
  suit_spades:      's',
  submit_buy:       'b',
  submit_sell:      'a',
  nudge_buy:        'ArrowUp',
  nudge_sell:       'ArrowDown',
  cancel_best_buy:  'Control+z',
  cancel_best_sell: 'Control+x',
  toggle_shortcuts: '?',
}

function renderPage() {
  return render(
    <MemoryRouter>
      <KeybindSettings />
    </MemoryRouter>
  )
}

beforeEach(() => {
  vi.clearAllMocks()
  vi.spyOn(useKeyBindsModule, 'useKeyBinds').mockReturnValue({
    binds: DEFAULT_BINDS,
    loading: false,
    update: mockUpdate,
  })
})

describe('KeybindSettings', () => {
  it('renders a row for each default action', () => {
    renderPage()
    expect(screen.getByText('Focus Clubs')).toBeInTheDocument()
    expect(screen.getByText('Focus Diamonds')).toBeInTheDocument()
    expect(screen.getByText('Focus Hearts')).toBeInTheDocument()
    expect(screen.getByText('Focus Spades')).toBeInTheDocument()
    expect(screen.getByText('Submit Buy')).toBeInTheDocument()
    expect(screen.getByText('Submit Sell')).toBeInTheDocument()
    expect(screen.getByText('Nudge Buy (+1)')).toBeInTheDocument()
    expect(screen.getByText('Nudge Sell (−1)')).toBeInTheDocument()
    expect(screen.getByText('Cancel Best Buy')).toBeInTheDocument()
    expect(screen.getByText('Cancel Best Sell')).toBeInTheDocument()
    expect(screen.getByText('Toggle Shortcut Help')).toBeInTheDocument()
  })

  it('shows loading state when binds are loading', () => {
    vi.spyOn(useKeyBindsModule, 'useKeyBinds').mockReturnValue({
      binds: {},
      loading: true,
      update: mockUpdate,
    })
    renderPage()
    expect(screen.getByText('Loading…')).toBeInTheDocument()
    expect(screen.queryByRole('table')).not.toBeInTheDocument()
  })

  it('clicking a row shows "Press a key…" prompt', () => {
    renderPage()
    const row = screen.getByText('Focus Clubs').closest('tr')!
    fireEvent.click(row)
    expect(screen.getByText('Press a key…')).toBeInTheDocument()
  })

  it('pressing a key while editing updates the binding display', () => {
    renderPage()
    fireEvent.click(screen.getByText('Focus Clubs').closest('tr')!)
    fireEvent.keyDown(document, { key: 'q', ctrlKey: false, altKey: false, shiftKey: false })
    // Editing ends and new key is shown
    expect(screen.queryByText('Press a key…')).not.toBeInTheDocument()
    expect(screen.getByText('q')).toBeInTheDocument()
  })

  it('modifier+key combination is serialized correctly', () => {
    renderPage()
    fireEvent.click(screen.getByText('Cancel Best Buy').closest('tr')!)
    // Simulate pressing Ctrl+k
    fireEvent.keyDown(document, { key: 'k', ctrlKey: true, altKey: false, shiftKey: false })
    expect(screen.queryByText('Press a key…')).not.toBeInTheDocument()
    expect(screen.getByText('Control+k')).toBeInTheDocument()
  })

  it('pure modifier keydown does not close editing mode', () => {
    renderPage()
    fireEvent.click(screen.getByText('Focus Clubs').closest('tr')!)
    // Pressing just Control should be ignored
    fireEvent.keyDown(document, { key: 'Control', ctrlKey: true, altKey: false, shiftKey: false })
    expect(screen.getByText('Press a key…')).toBeInTheDocument()
  })

  it('Save button calls update() with full bind list', async () => {
    mockUpdate.mockResolvedValue(undefined)
    renderPage()
    fireEvent.click(screen.getByRole('button', { name: 'Save' }))
    expect(mockUpdate).toHaveBeenCalledWith(
      expect.arrayContaining([
        expect.objectContaining({ action: 'suit_clubs', key_combo: 'c' }),
        expect.objectContaining({ action: 'cancel_best_buy', key_combo: 'Control+z' }),
      ])
    )
  })

  it('shows "Saved" confirmation after successful update', async () => {
    mockUpdate.mockResolvedValue(undefined)
    renderPage()
    fireEvent.click(screen.getByRole('button', { name: 'Save' }))
    await waitFor(() => expect(screen.getByText('Saved')).toBeInTheDocument())
  })

  it('shows error message on failed save', async () => {
    mockUpdate.mockRejectedValue(new Error('Network error'))
    renderPage()
    fireEvent.click(screen.getByRole('button', { name: 'Save' }))
    await waitFor(() => expect(screen.getByText('Save failed')).toBeInTheDocument())
  })

  it('Save button is disabled while saving', async () => {
    // Update never resolves — simulates slow network
    mockUpdate.mockReturnValue(new Promise(() => {}))
    renderPage()
    fireEvent.click(screen.getByRole('button', { name: 'Save' }))
    expect(screen.getByRole('button', { name: 'Saving…' })).toBeDisabled()
  })
})
