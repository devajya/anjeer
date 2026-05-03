import { useState, useEffect, useCallback, useRef } from 'react'
import { useNavigate } from 'react-router-dom'
import { useKeyBinds } from '../hooks/useKeyBinds'
import './KeybindSettings.css'

// AGENT-CTX: Action keys are stable DB identifiers — never rename them without a
// migration. Labels here are display-only and may change freely.
const ACTION_LABELS: Record<string, string> = {
  suit_clubs:       'Focus Clubs',
  suit_diamonds:    'Focus Diamonds',
  suit_hearts:      'Focus Hearts',
  suit_spades:      'Focus Spades',
  submit_buy:       'Submit Buy',
  submit_sell:      'Submit Sell',
  nudge_buy:        'Nudge Buy (+1)',
  nudge_sell:       'Nudge Sell (−1)',
  cancel_best_buy:  'Cancel Best Buy',
  cancel_best_sell: 'Cancel Best Sell',
  toggle_shortcuts: 'Toggle Shortcut Help',
}

// AGENT-CTX: Display order: suits first (motor memory), then trade actions, then utility.
const ACTION_ORDER = Object.keys(ACTION_LABELS)

function serializeKeyEvent(e: KeyboardEvent): string {
  // AGENT-CTX: Pure modifier presses are not valid bindings — return '' to skip.
  if (['Control', 'Alt', 'Shift', 'Meta'].includes(e.key)) return ''
  const parts: string[] = []
  if (e.ctrlKey)  parts.push('Control')
  if (e.altKey)   parts.push('Alt')
  if (e.shiftKey) parts.push('Shift')
  parts.push(e.key)
  return parts.join('+')
}

export function KeybindSettings() {
  const navigate = useNavigate()
  const { binds, loading, update } = useKeyBinds()

  // AGENT-CTX: localBinds is an in-progress edit copy — not synced to server until
  // Save is clicked. Initialized from hook binds once the API fetch resolves.
  const [localBinds, setLocalBinds] = useState<Record<string, string>>({})
  const [editingAction, setEditingAction] = useState<string | null>(null)
  const [saveState, setSaveState] = useState<'idle' | 'saving' | 'saved' | 'error'>('idle')

  // AGENT-CTX: hasInitialized guard prevents re-sync after user has started editing.
  const hasInitialized = useRef(false)
  useEffect(() => {
    if (!loading && !hasInitialized.current) {
      hasInitialized.current = true
      setLocalBinds({ ...binds })
    }
  }, [loading, binds])

  const handleRowClick = useCallback((action: string) => {
    setEditingAction(action)
    setSaveState('idle')
  }, [])

  // AGENT-CTX: Listener on document (not the row) so focus management is not required —
  // user clicks a row to start capture, then presses any key without needing to
  // maintain focus on the specific element.
  useEffect(() => {
    if (!editingAction) return

    function onKeyDown(e: KeyboardEvent) {
      e.preventDefault()
      const combo = serializeKeyEvent(e)
      if (!combo) return
      setLocalBinds(prev => ({ ...prev, [editingAction!]: combo }))
      setEditingAction(null)
    }

    document.addEventListener('keydown', onKeyDown)
    return () => document.removeEventListener('keydown', onKeyDown)
  }, [editingAction])

  async function handleSave() {
    setSaveState('saving')
    try {
      await update(
        Object.entries(localBinds).map(([action, key_combo]) => ({ action, key_combo }))
      )
      setSaveState('saved')
    } catch {
      setSaveState('error')
    }
  }

  return (
    <div className="keybinds">
      <header className="keybinds__header">
        <button className="keybinds__back" onClick={() => navigate('/lobby')}>
          ← Back
        </button>
        <h1 className="keybinds__title">Key Bindings</h1>
      </header>

      {loading ? (
        <p className="keybinds__loading">Loading…</p>
      ) : (
        <>
          <p className="keybinds__hint">Click a row, then press a key to rebind.</p>

          <table className="keybinds__table">
            <thead>
              <tr>
                <th className="keybinds__col-action">Action</th>
                <th className="keybinds__col-key">Key</th>
              </tr>
            </thead>
            <tbody>
              {ACTION_ORDER.map(action => {
                const isEditing = editingAction === action
                return (
                  <tr
                    key={action}
                    className={`keybinds__row${isEditing ? ' keybinds__row--editing' : ''}`}
                    onClick={() => handleRowClick(action)}
                    // AGENT-CTX: role=button so keyboard-only users can activate with Enter.
                    role="button"
                    tabIndex={0}
                    onKeyDown={e => { if (e.key === 'Enter') handleRowClick(action) }}
                  >
                    <td className="keybinds__action">{ACTION_LABELS[action]}</td>
                    <td className="keybinds__key">
                      {isEditing
                        ? <span className="keybinds__capture">Press a key…</span>
                        : <kbd className="keybinds__kbd">{localBinds[action] ?? '—'}</kbd>
                      }
                    </td>
                  </tr>
                )
              })}
            </tbody>
          </table>

          <footer className="keybinds__footer">
            <button
              className="keybinds__save"
              onClick={handleSave}
              disabled={saveState === 'saving'}
            >
              {saveState === 'saving' ? 'Saving…' : 'Save'}
            </button>
            {saveState === 'saved' && (
              <span className="keybinds__saved-msg">Saved</span>
            )}
            {saveState === 'error' && (
              <span className="keybinds__error-msg">Save failed</span>
            )}
          </footer>
        </>
      )}
    </div>
  )
}
