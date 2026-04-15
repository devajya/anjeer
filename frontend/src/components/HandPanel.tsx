import type { HandCounts } from '../types/messages'
import './HandPanel.css'

export type { HandCounts }

interface HandPanelProps {
  hand: HandCounts | null
  initialHand?: HandCounts | null
}

const SUITS = [
  { key: 'clubs'    as const, label: 'Clubs',    symbol: '♣', color: 'black' },
  { key: 'diamonds' as const, label: 'Diamonds', symbol: '♦', color: 'red'   },
  { key: 'hearts'   as const, label: 'Hearts',   symbol: '♥', color: 'red'   },
  { key: 'spades'   as const, label: 'Spades',   symbol: '♠', color: 'black' },
]

// AGENT-CTX: Shows only counts, not ranks — cards are fungible within a suit
// until a future scoring slice requires rank-level display (Slice 3 resolution 7).
export function HandPanel({ hand, initialHand }: HandPanelProps) {
  if (!hand) {
    return (
      <div className="hand-panel hand-panel--waiting">
        Waiting for round to start…
      </div>
    )
  }

  return (
    <div className="hand-panel">
      <h3 className="hand-panel__title">My Hand</h3>
      <ul className="hand-panel__list">
        {SUITS.map(({ key, label, symbol, color }) => {
          const delta = (initialHand != null) ? hand[key] - initialHand[key] : 0
          return (
            <li key={key} className={`hand-panel__row hand-panel__row--${color}`}>
              <span className="hand-panel__symbol">{symbol}</span>
              <span className="hand-panel__label">{label}</span>
              <span className="hand-panel__count">{hand[key]}</span>
              {delta !== 0 && (
                <span
                  className={`hand-panel__delta hand-panel__delta--${delta > 0 ? 'up' : 'down'}`}
                  aria-label={`${delta > 0 ? 'gained' : 'lost'} ${Math.abs(delta)}`}
                >
                  {delta > 0 ? '▲' : '▼'} {delta > 0 ? `+${delta}` : delta}
                </span>
              )}
            </li>
          )
        })}
      </ul>
    </div>
  )
}
