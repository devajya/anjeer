import type { TradeEntry } from '../hooks/useWebSocket'
import './TradeFeed.css'

interface Props {
  trades: TradeEntry[]
}

// AGENT-CTX: Renders a rolling list of executed trades, newest first.
// your_side is null for observers; when non-null the row is highlighted so
// the player immediately sees trades they were party to.
// No counterparty identity is revealed — consistent with Slice 8 spec
// ("trade history shows price, quantity, suit — no player identity revealed").
export function TradeFeed({ trades }: Props) {
  return (
    <div className="trade-feed">
      <h2 className="trade-feed__title">Trades</h2>
      {trades.length === 0 ? (
        <p className="trade-feed__empty">No trades yet.</p>
      ) : (
        <ul className="trade-feed__list">
          {trades.map(t => (
            <li
              key={t.id}
              className={[
                'trade-feed__item',
                t.your_side ? 'trade-feed__item--mine' : '',
              ].join(' ').trim()}
            >
              <span className="trade-feed__suit">{t.suit}</span>
              <span className="trade-feed__price">@ {t.price}</span>
              <span
                className={[
                  'trade-feed__side',
                  t.aggressor_side === 'buy'
                    ? 'trade-feed__side--buy'
                    : 'trade-feed__side--sell',
                ].join(' ')}
              >
                {t.aggressor_side.toUpperCase()}
              </span>
              {t.your_side && (
                <span className="trade-feed__you">
                  you {t.your_side}
                </span>
              )}
            </li>
          ))}
        </ul>
      )}
    </div>
  )
}
