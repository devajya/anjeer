import { describe, test, expect, vi, beforeEach } from 'vitest'
import { render, screen } from '@testing-library/react'
import { MemoryRouter } from 'react-router-dom'
import { SettingsPage } from '../SettingsPage'
import * as useKeyBindsModule from '../../hooks/useKeyBinds'

const DEFAULT_BINDS: Record<string, string> = {
  suit_clubs: 'c', suit_diamonds: 'd', suit_hearts: 'h', suit_spades: 's',
  submit_buy: 'b', submit_sell: 'a', nudge_buy: 'ArrowUp', nudge_sell: 'ArrowDown',
  cancel_best_buy: 'Control+z', cancel_best_sell: 'Control+x', toggle_shortcuts: '?',
}

beforeEach(() => {
  vi.clearAllMocks()
  vi.spyOn(useKeyBindsModule, 'useKeyBinds').mockReturnValue({
    binds: DEFAULT_BINDS,
    loading: false,
    update: vi.fn(),
  })
  vi.stubGlobal('fetch', vi.fn().mockResolvedValue({ ok: true }))
})

function renderSettings(path = '/settings/keybinds') {
  return render(
    <MemoryRouter initialEntries={[path]}>
      <SettingsPage />
    </MemoryRouter>
  )
}

describe('SettingsPage', () => {
  test('renders keybind content', () => {
    renderSettings()
    expect(screen.getByText(/Focus Clubs/i)).toBeInTheDocument()
  })
})
