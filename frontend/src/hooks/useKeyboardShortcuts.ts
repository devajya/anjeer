// AGENT-CTX: Global keydown listener attached to window — avoids needing a
// focused element, which is essential for a trading UI where the user may not
// have clicked any input. Cleaned up on unmount or when deps change.
import { useEffect, useMemo } from 'react'

export interface UseKeyboardShortcutsOptions {
  binds: Record<string, string>
  // AGENT-CTX: enabled=false silences all shortcuts during inter-round overlays
  // and round-end modals so accidental keypresses don't queue orders.
  enabled: boolean
  onSuitFocus: (suit: string) => void
  onSubmitBuy: () => void
  onSubmitSell: () => void
  onAcceptBuy: () => void
  onAcceptSell: () => void
  onNudgeBuy: () => void
  onNudgeSell: () => void
  onCancelBestBuy: () => void
  onCancelBestSell: () => void
  onToggleShortcuts: () => void
  onFocusTradeFeed?:   () => void
  onFocusCenterPanel?: () => void
  onTogglePanel?:      () => void
}

// AGENT-CTX: Serializes a KeyboardEvent into the same canonical format used in
// the DB and DEFAULT_BINDS: "Modifier+key" where Modifier is Control/Shift/Alt
// and key is event.key verbatim. Multiple modifiers are joined alphabetically
// (Alt < Control < Shift) to ensure a unique canonical form per combination.
export function serializeCombo(e: KeyboardEvent): string {
  const mods: string[] = []
  if (e.altKey)     mods.push('Alt')
  if (e.ctrlKey)    mods.push('Control')
  if (e.shiftKey)   mods.push('Shift')
  // Modifier-only keypresses (e.g. pressing Ctrl alone) are not valid combos.
  if (['Alt', 'Control', 'Shift', 'Meta'].includes(e.key)) return ''
  return mods.length > 0 ? `${mods.join('+')}+${e.key}` : e.key
}


export function useKeyboardShortcuts(opts: UseKeyboardShortcutsOptions): void {
  const {
    binds, enabled,
    onSuitFocus, onSubmitBuy, onSubmitSell, onAcceptBuy, onAcceptSell,
    onNudgeBuy, onNudgeSell,
    onCancelBestBuy, onCancelBestSell,
    onToggleShortcuts,
    onFocusTradeFeed,
    onFocusCenterPanel,
    onTogglePanel,
  } = opts

  // AGENT-CTX: Inverted map built once per binds change (useMemo, not useCallback —
  // this computes a value). Maps key_combo → action. If two actions share the same
  // combo the last one in iteration wins; this is a user misconfiguration.
  const comboToAction = useMemo(() => {
    const map: Record<string, string> = {}
    for (const [action, combo] of Object.entries(binds)) {
      if (combo) map[combo] = action
    }
    return map
  }, [binds])

  useEffect(() => {
    if (!enabled) return

    const dispatch: Record<string, () => void> = {
      suit_clubs:          () => onSuitFocus('clubs'),
      suit_diamonds:       () => onSuitFocus('diamonds'),
      suit_hearts:         () => onSuitFocus('hearts'),
      suit_spades:         () => onSuitFocus('spades'),
      submit_buy:          onSubmitBuy,
      submit_sell:         onSubmitSell,
      accept_buy:          onAcceptBuy,
      accept_sell:         onAcceptSell,
      nudge_buy:           onNudgeBuy,
      nudge_sell:          onNudgeSell,
      cancel_best_buy:     onCancelBestBuy,
      cancel_best_sell:    onCancelBestSell,
      toggle_shortcuts:    onToggleShortcuts,
      focus_trade_feed:    onFocusTradeFeed   ?? (() => {}),
      focus_center_panel:  onFocusCenterPanel ?? (() => {}),
      toggle_panel:        onTogglePanel      ?? (() => {}),
    }

    function handleKeyDown(e: KeyboardEvent) {
      // AGENT-CTX: Skip when focus is inside a text input so typing prices
      // doesn't fire shortcuts. contenteditable is included for completeness.
      const tag = (e.target as HTMLElement)?.tagName
      if (tag === 'INPUT' || tag === 'TEXTAREA' || (e.target as HTMLElement)?.isContentEditable) return

      const combo = serializeCombo(e)
      if (!combo) return

      const action = comboToAction[combo]
      const handler = action ? dispatch[action] : undefined
      if (!handler) return

      // Only preventDefault for actions we handle — lets unbound browser
      // shortcuts (Ctrl+R, Ctrl+T, etc.) pass through untouched.
      e.preventDefault()
      handler()
    }

    window.addEventListener('keydown', handleKeyDown)
    return () => window.removeEventListener('keydown', handleKeyDown)
  }, [
    enabled, comboToAction,
    onSuitFocus, onSubmitBuy, onSubmitSell, onAcceptBuy, onAcceptSell,
    onNudgeBuy, onNudgeSell,
    onCancelBestBuy, onCancelBestSell,
    onToggleShortcuts, onFocusTradeFeed, onFocusCenterPanel, onTogglePanel,
  ])
}
