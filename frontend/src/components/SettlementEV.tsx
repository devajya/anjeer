import type { EvalPosteriorUpdateMessage } from '../types/messages'
import './SettlementEV.css'

const SUIT_SYMBOLS: Record<string, string> = {
  clubs: '♣', diamonds: '♦', hearts: '♥', spades: '♠',
}
const SUIT_COLORS: Record<string, string> = {
  clubs: '#2a7a2a', diamonds: '#1a5fc8', hearts: '#c0392b', spades: '#1a1a2e',
}
const SUITS = ['clubs', 'diamonds', 'hearts', 'spades']

interface Props {
  posteriorUpdate: EvalPosteriorUpdateMessage | null
}

export function SettlementEV({ posteriorUpdate }: Props) {
  if (!posteriorUpdate) {
    return (
      <div className="settlement-ev settlement-ev--empty" data-testid="settlement-ev">
        <span className="settlement-ev__placeholder">No EV estimate yet</span>
      </div>
    )
  }

  const { settlement_ev, delta_ev } = posteriorUpdate

  return (
    <div className="settlement-ev" data-testid="settlement-ev">
      <h4 className="settlement-ev__heading">Settlement EV</h4>
      <div className="settlement-ev__total" data-testid="settlement-ev-total">
        {settlement_ev.toFixed(1)}
      </div>

      <h4 className="settlement-ev__heading">Marginal Card EV</h4>
      <div className="settlement-ev__deltas" data-testid="settlement-ev-deltas">
        {SUITS.map(s => {
          const val = delta_ev[s] ?? 0
          const positive = val >= 0
          return (
            <div key={s} className="settlement-ev__delta-cell">
              <span
                className="settlement-ev__suit"
                style={{ color: SUIT_COLORS[s] }}
              >
                {SUIT_SYMBOLS[s]}
              </span>
              <span
                className={`settlement-ev__delta-val${positive ? ' settlement-ev__delta-val--pos' : ' settlement-ev__delta-val--neg'}`}
                data-testid={`delta-ev-${s}`}
              >
                {positive ? '+' : ''}{val.toFixed(1)}
              </span>
            </div>
          )
        })}
      </div>
    </div>
  )
}
