import { useEffect } from 'react'
import './ShortcutHelp.css'

// AGENT-CTX: Action labels shown to the user — kept here rather than in
// useKeyBinds so the hook stays data-only. Order matches DEFAULT_BINDS key
// order to keep the overlay grouped by category (suits → orders → cancel).
const ACTION_LABELS: Record<string, string> = {
  suit_clubs:       'Focus Clubs',
  suit_diamonds:    'Focus Diamonds',
  suit_hearts:      'Focus Hearts',
  suit_spades:      'Focus Spades',
  submit_buy:       'Submit Buy',
  submit_sell:      'Submit Sell',
  accept_buy:       'Accept Buy (hit bid)',
  accept_sell:      'Accept Sell (lift offer)',
  nudge_buy:        'Nudge Buy +1',
  nudge_sell:       'Nudge Sell −1',
  cancel_best_buy:  'Cancel Best Buy',
  cancel_best_sell: 'Cancel Best Sell',
  toggle_shortcuts: 'Toggle This Panel',
}

interface Props {
  binds: Record<string, string>
  onClose: () => void
}

export function ShortcutHelp({ binds, onClose }: Props) {
  // AGENT-CTX: Escape also closes the overlay so keyboard-only users can
  // dismiss without reaching for the mouse.
  useEffect(() => {
    function handleKey(e: KeyboardEvent) {
      if (e.key === 'Escape') {
        e.preventDefault()
        onClose()
      }
    }
    window.addEventListener('keydown', handleKey)
    return () => window.removeEventListener('keydown', handleKey)
  }, [onClose])

  return (
    <div className="shortcut-help__backdrop" onClick={onClose} aria-modal="true" role="dialog">
      {/* AGENT-CTX: stopPropagation so clicks inside the panel don't
          bubble to the backdrop and close it. */}
      <div
        className="shortcut-help__panel"
        onClick={e => e.stopPropagation()}
      >
        <div className="shortcut-help__header">
          <span className="shortcut-help__title">Keyboard Shortcuts</span>
          <button
            className="shortcut-help__close"
            onClick={onClose}
            aria-label="Close shortcuts panel"
          >
            ✕
          </button>
        </div>
        <table className="shortcut-help__table">
          <tbody>
            {Object.entries(ACTION_LABELS).map(([action, label]) => (
              <tr key={action} className="shortcut-help__row">
                <td className="shortcut-help__action">{label}</td>
                <td className="shortcut-help__key">
                  <kbd>{binds[action] ?? '—'}</kbd>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  )
}
