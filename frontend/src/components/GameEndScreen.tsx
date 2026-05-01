import { useNavigate } from 'react-router-dom'
import type { GameEndedMessage } from '../types/messages'
import './GameEndScreen.css'

const SUIT_ICON: Record<string, string> = {
  clubs: '♣', diamonds: '♦', hearts: '♥', spades: '♠',
}

interface GameEndScreenProps {
  gameEnded: GameEndedMessage
  // AGENT-CTX: playerSlot identifies the local player's row for highlighting.
  // null before round_start has been received (no slot assigned yet); safe —
  // === comparison with null never matches a valid slot number.
  playerSlot: number | null
}

export function GameEndScreen({ gameEnded, playerSlot }: GameEndScreenProps) {
  const navigate = useNavigate()

  return (
    <div
      className="ges__backdrop"
      role="dialog"
      aria-modal="true"
      aria-label="Game over"
    >
      <div className="ges__card">
        <h2 className="ges__title">Game Over</h2>

        {/* ── Final standings ── */}
        <section className="ges__section">
          <h3 className="ges__section-title">Final Standings</h3>
          <table className="ges__table" aria-label="Final standings">
            <thead>
              <tr>
                <th>Player</th>
                <th>Balance</th>
                <th>Net</th>
              </tr>
            </thead>
            <tbody>
              {gameEnded.final_standings.map(s => (
                <tr
                  key={s.player_slot}
                  className={[
                    'ges__row',
                    s.player_slot === playerSlot ? 'ges__row--own' : '',
                  ].filter(Boolean).join(' ')}
                >
                  <td>{s.username}</td>
                  <td>{s.final_balance}</td>
                  <td className={s.net_change >= 0 ? 'ges__net--pos' : 'ges__net--neg'}>
                    {s.net_change >= 0 ? '+' : ''}{s.net_change}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </section>

        {/* ── Per-round breakdown ── */}
        <section className="ges__section" aria-label="Round breakdown">
          <h3 className="ges__section-title">Round Breakdown</h3>
          {gameEnded.rounds.map(round => (
            <div key={round.round_number} className="ges__round">
              <div className="ges__round-header">
                <span className="ges__round-num">Round {round.round_number}</span>
                <span
                  className={`ges__suit-icon ges__suit-icon--${round.goal_suit}`}
                  aria-hidden="true"
                >
                  {SUIT_ICON[round.goal_suit] ?? round.goal_suit}
                </span>
                <span className="ges__round-suit">
                  {round.goal_suit.charAt(0).toUpperCase() + round.goal_suit.slice(1)}
                </span>
              </div>
              <table className="ges__table ges__table--compact">
                <thead>
                  <tr>
                    <th>Player</th>
                    <th>Goal Cards</th>
                    <th>Payout</th>
                    <th>Balance</th>
                  </tr>
                </thead>
                <tbody>
                  {round.results.map(r => (
                    <tr
                      key={r.player_slot}
                      className={[
                        'ges__row',
                        r.player_slot === playerSlot ? 'ges__row--own' : '',
                      ].filter(Boolean).join(' ')}
                    >
                      <td>
                        Player {r.player_slot}
                        {r.disconnected && <span className="ges__away"> (away)</span>}
                      </td>
                      <td>{r.goal_cards_held}</td>
                      <td>{r.payout}</td>
                      <td>{r.balance}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          ))}
        </section>

        <button className="ges__return-btn" onClick={() => navigate('/lobby')}>
          Return to Lobby
        </button>
      </div>
    </div>
  )
}
