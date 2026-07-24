import { memo } from 'react'
import type { TradeEntry } from '../hooks/useWebSocket'
import { SUIT_SYMBOLS as SUIT_SYMBOL, suitClass } from '../utils/suits'
import './TradeFeed.css'

interface RosterEntry {
  player_slot: number
  username: string
}

interface Props {
  trades: TradeEntry[]
  // AGENT-CTX: roster is populated from round_start; empty before first round.
  // When empty, slot numbers are shown as fallback ("Slot 0") so the feed is
  // never blank even if roster hasn't arrived yet.
  roster: RosterEntry[]
}

function slotName(slot: number, roster: RosterEntry[]): string {
  return roster.find(r => r.player_slot === slot)?.username ?? `Slot ${slot}`
}

export const TradeFeed = memo(function TradeFeed({ trades, roster }: Props) {
  return (
    <div className="trade-feed">
      {trades.length === 0 ? (
        <p className="trade-feed__empty">No trades yet.</p>
      ) : (
        <table className="trade-feed__table">
          <thead>
            <tr className="trade-feed__header">
              <th className="trade-feed__th trade-feed__th--buyer">Buyer</th>
              <th className="trade-feed__th trade-feed__th--suit">Suit</th>
              <th className="trade-feed__th trade-feed__th--seller">Seller</th>
              <th className="trade-feed__th trade-feed__th--price">Price</th>
            </tr>
          </thead>
          <tbody>
            {trades.map(t => (
              <tr
                key={t.id}
                className={[
                  'trade-feed__row',
                  t.your_side ? 'trade-feed__row--mine' : '',
                ].join(' ').trim()}
              >
                <td className="trade-feed__td trade-feed__td--buyer">
                  {slotName(t.buyer_slot, roster)}
                </td>
                <td className={`trade-feed__td trade-feed__td--suit ${suitClass(t.suit)}`}>
                  {SUIT_SYMBOL[t.suit] ?? t.suit}
                </td>
                <td className="trade-feed__td trade-feed__td--seller">
                  {slotName(t.seller_slot, roster)}
                </td>
                <td className="trade-feed__td trade-feed__td--price">
                  {t.price}
                  <span className="trade-feed__qty">
                    {t.qty_filled}/{t.qty_ordered}
                  </span>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
    </div>
  )
})
