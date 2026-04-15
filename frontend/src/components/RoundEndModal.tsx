import type { RoundEndMessage } from '../types/messages'
import './RoundEndModal.css'

// AGENT-CTX: Suit glyphs are Unicode characters, not images or an icon library,
// to keep the bundle lean. They render correctly in all target browsers and
// survive copy-paste (important for API players logging results).
const SUIT_ICON: Record<string, string> = {
  clubs:    '♣',
  diamonds: '♦',
  hearts:   '♥',
  spades:   '♠',
}

interface RoundEndModalProps {
  roundEnd:   RoundEndMessage
  // AGENT-CTX: playerSlot can be null if round_end somehow arrives before
  // round_start (edge case in Waiting→Active transition). Guard throughout
  // so the modal still renders without a highlighted own row in that case.
  playerSlot: number | null
  onDismiss:  () => void
}

export function RoundEndModal({ roundEnd, playerSlot, onDismiss }: RoundEndModalProps) {
  const ownResult = roundEnd.results.find(r => r.player_slot === playerSlot) ?? null

  // Capitalise the first letter of the goal_suit name for display.
  const goalSuitDisplay =
    roundEnd.goal_suit.charAt(0).toUpperCase() + roundEnd.goal_suit.slice(1)

  return (
    // AGENT-CTX: Clicking the backdrop dismisses the modal; clicking inside the
    // card stops propagation so the card itself never triggers onDismiss.
    <div
      className="round-end-backdrop"
      onClick={onDismiss}
      role="dialog"
      aria-modal="true"
      aria-label="Round results"
    >
      <div
        className="round-end-modal"
        onClick={(e) => e.stopPropagation()}
      >
        <h2 className="round-end-modal__title">Round Over</h2>

        {/* ── Goal suit reveal ── */}
        <div className="round-end-modal__goal-suit">
          <span
            className={`round-end-modal__suit-icon round-end-modal__suit-icon--${roundEnd.goal_suit}`}
            aria-hidden="true"
          >
            {SUIT_ICON[roundEnd.goal_suit] ?? roundEnd.goal_suit}
          </span>
          <span className="round-end-modal__suit-name">
            {goalSuitDisplay} was the goal suit
          </span>
        </div>

        {/* ── Own summary — only rendered when playerSlot is known ── */}
        {ownResult !== null && (
          <div className="round-end-modal__own-summary">
            <span>
              Payout: <strong>{ownResult.payout}</strong>
            </span>
            <span>
              New balance: <strong>{ownResult.new_balance}</strong>
            </span>
          </div>
        )}

        {/* ── Full standings table ── */}
        <table className="round-end-modal__standings">
          <thead>
            <tr>
              <th>Player</th>
              <th>Goal Cards</th>
              <th>Payout</th>
              <th>Balance</th>
            </tr>
          </thead>
          <tbody>
            {roundEnd.results.map((r) => (
              <tr
                key={r.player_slot}
                className={[
                  'round-end-modal__row',
                  r.player_slot === playerSlot ? 'round-end-modal__row--own' : '',
                ]
                  .filter(Boolean)
                  .join(' ')}
              >
                <td>
                  Player {r.player_slot}
                  {r.disconnected && (
                    <span className="round-end-modal__away"> (away)</span>
                  )}
                </td>
                <td>{r.goal_cards_held}</td>
                <td>{r.payout}</td>
                <td>{r.new_balance}</td>
              </tr>
            ))}
          </tbody>
        </table>

        <button className="round-end-modal__dismiss" onClick={onDismiss}>
          Dismiss
        </button>
      </div>
    </div>
  )
}
