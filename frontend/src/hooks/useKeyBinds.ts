import { useState, useEffect, useCallback, useMemo } from 'react'

export interface KeyBind {
  action: string
  key_combo: string
}

// AGENT-CTX: DEFAULT_BINDS is the canonical fallback for every action.
// Keys use event.key values (case-sensitive). Modifier prefix convention:
// "Control+", "Shift+", "Alt+" — must match serializeCombo() in KeybindSettings.
export const DEFAULT_BINDS: Record<string, string> = {
  suit_clubs:       'c',
  suit_diamonds:    'd',
  suit_hearts:      'h',
  suit_spades:      's',
  submit_buy:       'b',
  submit_sell:      'a',
  accept_buy:       'Shift+B',
  accept_sell:      'Shift+A',
  nudge_buy:        'ArrowUp',
  nudge_sell:       'ArrowDown',
  cancel_best_buy:    'Control+z',
  cancel_best_sell:   'Control+x',
  toggle_shortcuts:   '?',
  focus_trade_feed:   'f',
  focus_center_panel: 'g',
  toggle_panel:       '`',
}

export interface UseKeyBindsResult {
  /** Merged map: defaults overridden by any saved DB values. */
  binds: Record<string, string>
  loading: boolean
  /** Full replace — sends all current binds to PUT /players/me/keybinds. */
  update(binds: KeyBind[]): Promise<void>
}

// AGENT-CTX: On 401/403 or network error the hook silently falls back to
// DEFAULT_BINDS so the game remains playable without a session. update() will
// also fail silently — KeybindSettings is responsible for surfacing save errors.
export function useKeyBinds(): UseKeyBindsResult {
  const [saved, setSaved] = useState<KeyBind[] | null>(null)
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    let cancelled = false
    fetch('/players/me/keybinds', { credentials: 'include' })
      .then(res => (res.ok ? (res.json() as Promise<KeyBind[]>) : null))
      .then(data => { if (!cancelled) setSaved(data) })
      .catch(() => { if (!cancelled) setSaved(null) })
      .finally(() => { if (!cancelled) setLoading(false) })
    return () => { cancelled = true }
  }, [])

  // AGENT-CTX: Merge strategy — start from defaults, overwrite with DB values
  // for known actions only. Unknown DB actions are ignored so stale rows for
  // removed actions don't corrupt the bind map. useMemo keeps the reference
  // stable across renders so useKeyboardShortcuts doesn't re-register unnecessarily.
  const binds = useMemo(() => {
    const merged = { ...DEFAULT_BINDS }
    if (saved) {
      for (const { action, key_combo } of saved) {
        if (action in DEFAULT_BINDS) merged[action] = key_combo
      }
    }
    return merged
  }, [saved])

  const update = useCallback(async (nextBinds: KeyBind[]) => {
    await fetch('/players/me/keybinds', {
      method: 'PUT',
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(nextBinds),
    })
    setSaved(nextBinds)
  }, [])

  return { binds, loading, update }
}
