import { describe, test, expect, vi, beforeEach } from 'vitest'
import { render, screen, fireEvent, waitFor } from '@testing-library/react'
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
  test('/settings renders subpage navigation', () => {
    renderSettings()
    expect(screen.getByRole('link', { name: /keybinds/i })).toBeInTheDocument()
    expect(screen.getByRole('link', { name: /preferences/i })).toBeInTheDocument()
  })

  test('keybinds subpage renders keybind content by default', () => {
    renderSettings('/settings/keybinds')
    expect(screen.getByText(/Focus Clubs/i)).toBeInTheDocument()
  })

  test('preferences subpage renders feed preference cards', () => {
    renderSettings('/settings/preferences')
    expect(screen.getByLabelText(/Select MBP-1 feed/i)).toBeInTheDocument()
    expect(screen.getByLabelText(/Select MBP-N feed/i)).toBeInTheDocument()
    expect(screen.getByLabelText(/Select MBO feed/i)).toBeInTheDocument()
  })
})

describe('FeedPreferenceSection', () => {
  test('FeedPreferenceSection shows MBP1, MBPN, MBO cards', () => {
    renderSettings('/settings/preferences')
    expect(screen.getByText('MBP-1')).toBeInTheDocument()
    expect(screen.getByText('MBP-N')).toBeInTheDocument()
    expect(screen.getByText('MBO')).toBeInTheDocument()
  })

  test('selecting feed card fires PUT /players/me/feed', async () => {
    renderSettings('/settings/preferences')
    fireEvent.click(screen.getByLabelText(/Select MBP-N feed/i))
    await waitFor(() => {
      expect(fetch).toHaveBeenCalledWith(
        '/players/me/feed',
        expect.objectContaining({
          method: 'PUT',
          body: JSON.stringify({ feed_preference: 'mbpn' }),
        })
      )
    })
  })
})
