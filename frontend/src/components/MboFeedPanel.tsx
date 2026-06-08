import { useMemo } from 'react'
import type { MboLogEntry, MyOrder } from '../hooks/useWebSocket'
import { SUIT_SYMBOLS, SUIT_ORDER, suitClass } from '../utils/suits'
import { slotColor } from '../utils/playerColors'
import './MboFeedPanel.css'

interface RosterEntry { player_slot: number; username: string }

interface Props {
  mboLogs:     Record<string, MboLogEntry[]>
  myOrders:    MyOrder[]
  onCancel:    (orderId: number) => void
  roster?:     RosterEntry[]
  playerSlot?: number | null
}

// ── Synthetic book for the price ladder ───────────────────────────────────────
interface SyntheticSuit {
  bids: { price: number; count: number }[]
  asks: { price: number; count: number }[]
}

function useSyntheticBook(
  mboLogs:  Record<string, MboLogEntry[]>,
  myOrders: MyOrder[],
): Record<string, SyntheticSuit> {
  const myIds = useMemo(() => new Set(myOrders.map(o => `${o.suit}:${o.order_id}`)), [myOrders])
  return useMemo(() => {
    const out: Record<string, SyntheticSuit> = {}
    for (const suit of SUIT_ORDER) {
      const entries = mboLogs[suit] ?? []
      const active  = new Map<number, { side: 'buy' | 'sell'; price: number }>()
      const removed = new Set<number>()
      for (const e of entries.slice().reverse()) {
        if (removed.has(e.order_id)) continue
        if (e.kind === 'added' && e.side && e.price !== null) {
          active.set(e.order_id, { side: e.side, price: e.price })
        } else if (e.kind === 'executed' || e.kind === 'cancelled') {
          removed.add(e.order_id)
          active.delete(e.order_id)
        }
      }
      const bidMap = new Map<number, number>()
      const askMap = new Map<number, number>()
      for (const [, { side, price }] of active) {
        const m = side === 'buy' ? bidMap : askMap
        m.set(price, (m.get(price) ?? 0) + 1)
      }
      void myIds
      out[suit] = {
        bids: [...bidMap].map(([price, count]) => ({ price, count })).sort((a, b) => b.price - a.price),
        asks: [...askMap].map(([price, count]) => ({ price, count })).sort((a, b) => a.price - b.price),
      }
    }
    return out
  }, [mboLogs, myIds])
}

// ── Price ladder ──────────────────────────────────────────────────────────────
function PriceLadder({ synBook }: { synBook: Record<string, SyntheticSuit> }) {
  return (
    <div className="mbo-vis mbo-vis--ladder">
      {SUIT_ORDER.map(suit => {
        const s = synBook[suit]
        if (!s) return null
        const maxCount = Math.max(1, ...s.bids.map(b => b.count), ...s.asks.map(a => a.count))
        return (
          <div key={suit} className="mbo-vis__suit">
            <div className="mbo-vis__suit-hdr">
              <span className={`mbo-vis__symbol ${suitClass(suit)}`}>{SUIT_SYMBOLS[suit]}</span>
              <span className="mbo-vis__name">{suit}</span>
            </div>
            <div className="mbo-vis__ladder-grid">
              <div className="mbo-vis__col mbo-vis__col--bid">
                {s.bids.slice(0, 4).map(lvl => (
                  <div key={lvl.price} className="mbo-vis__level mbo-vis__level--bid">
                    <div className="mbo-vis__bar mbo-vis__bar--bid"
                      style={{ width: `${(lvl.count / maxCount) * 100}%` }} />
                    <span className="mbo-vis__count">{lvl.count}</span>
                    <span className="mbo-vis__price">{lvl.price}</span>
                  </div>
                ))}
                {s.bids.length === 0 && <span className="mbo-vis__empty">—</span>}
              </div>
              <div className="mbo-vis__col mbo-vis__col--ask">
                {s.asks.slice(0, 4).map(lvl => (
                  <div key={lvl.price} className="mbo-vis__level mbo-vis__level--ask">
                    <span className="mbo-vis__price">{lvl.price}</span>
                    <span className="mbo-vis__count">{lvl.count}</span>
                    <div className="mbo-vis__bar mbo-vis__bar--ask"
                      style={{ width: `${(lvl.count / maxCount) * 100}%` }} />
                  </div>
                ))}
                {s.asks.length === 0 && <span className="mbo-vis__empty">—</span>}
              </div>
            </div>
          </div>
        )
      })}
    </div>
  )
}

// ── Order summary model ───────────────────────────────────────────────────────
interface OrderSummary {
  order_id:   number
  seq:        number
  side:       'buy' | 'sell'
  price:      number
  qty:        number
  qty_filled: number
  status:     'resting' | 'filled' | 'cancelled'
  owner_slot: number | null
  isMine:     boolean
  isLive:     boolean
}

function buildOrderSummaries(
  entries:    MboLogEntry[],
  suit:       string,
  liveIds:    Set<string>,
  playerSlot: number | null,
): OrderSummary[] {
  const orders = new Map<number, OrderSummary>()

  for (const e of entries.slice().reverse()) {
    if (e.kind === 'added' && e.side && e.price !== null) {
      orders.set(e.order_id, {
        order_id:   e.order_id,
        seq:        e.seq,
        side:       e.side,
        price:      e.price,
        qty:        e.qty ?? 1,
        qty_filled: 0,
        status:     'resting',
        owner_slot: e.owner_slot ?? null,
        // isMine derived from server-provided owner_slot — always authoritative,
        // no accumulation or stale-ref issues.
        isMine:     playerSlot !== null && e.owner_slot !== undefined && e.owner_slot === playerSlot,
        isLive:     false,
      })
    } else if (e.kind === 'executed') {
      const fillAmt = e.qty_filled ?? 1
      const passive = orders.get(e.order_id)
      if (passive) {
        passive.qty_filled += fillAmt
        if (passive.qty_filled >= passive.qty) passive.status = 'filled'
      }
      if (e.aggressor_order_id != null) {
        const aggressor = orders.get(e.aggressor_order_id)
        if (aggressor) {
          aggressor.qty_filled += fillAmt
          if (aggressor.qty_filled >= aggressor.qty) aggressor.status = 'filled'
        }
      }
    } else if (e.kind === 'cancelled') {
      const s = orders.get(e.order_id)
      if (s) s.status = 'cancelled'
    }
  }

  // isLive = order is in myOrders (qty_remaining > 0, not cancelled).
  // myOrders is the single source of truth for cancel eligibility; the MBO
  // status field is only used for visual fill state, not for gating the button.
  for (const [id, s] of orders) {
    const key = `${suit}:${id}`
    s.isLive = liveIds.has(key)
  }

  return [...orders.values()]
    .filter(s => !(s.status === 'cancelled' && s.qty_filled === 0))
    .sort((a, b) => {
      // Resting mine first (most actionable), then by seq descending
      if (a.isLive !== b.isLive) return a.isLive ? -1 : 1
      return b.seq - a.seq
    })
}

// ── Order feed ────────────────────────────────────────────────────────────────
function OrderFeed({
  mboLogs,
  myOrders,
  onCancel,
  roster,
  playerSlot,
}: {
  mboLogs:     Record<string, MboLogEntry[]>
  myOrders:    MyOrder[]
  onCancel:    (orderId: number) => void
  roster?:     RosterEntry[]
  playerSlot?: number | null
}) {
  const liveIds = useMemo(() => new Set(myOrders.map(o => `${o.suit}:${o.order_id}`)), [myOrders])

  return (
    <div className="mbo-log">
      {SUIT_ORDER.map(suit => {
        const entries  = mboLogs[suit] ?? []
        const summaries = buildOrderSummaries(entries, suit, liveIds, playerSlot ?? null)
        return (
          <div key={suit} className="mbo-log__section">
            <div className="mbo-log__hdr">
              <span className={`mbo-log__symbol ${suitClass(suit)}`}>{SUIT_SYMBOLS[suit]}</span>
              <span className="mbo-log__name">{suit}</span>
            </div>
            <div className="mbo-log__feed">
              {summaries.length === 0 ? (
                <div className="mbo-log__empty">no orders</div>
              ) : (
                summaries.map(s => {
                  const fillPct = s.qty > 0 ? (s.qty_filled / s.qty) * 100 : 0
                  const isFullFill = s.status === 'filled'
                  const displayPct = isFullFill ? 100 : fillPct

                  const username = s.owner_slot !== null && s.owner_slot >= 0
                    ? (roster?.find(r => r.player_slot === s.owner_slot)?.username ?? null)
                    : null
                  const isMe = s.owner_slot !== null && s.owner_slot === playerSlot

                  const whoLabel  = isMe ? 'you' : username
                  const whoColor  = s.owner_slot !== null && s.owner_slot >= 0
                    ? slotColor(s.owner_slot)
                    : undefined

                  return (
                    <div
                      key={s.order_id}
                      className={[
                        'mbo-order',
                        `mbo-order--${s.status}`,
                        s.isMine  ? 'mbo-order--mine'     : '',
                        s.isLive  ? 'mbo-order--live'     : '',
                      ].filter(Boolean).join(' ')}
                      style={{ '--fill-pct': `${displayPct}%` } as React.CSSProperties}
                      onClick={s.isLive ? () => onCancel(s.order_id) : undefined}
                      title={s.isLive ? 'Click to cancel' : undefined}
                    >
                      <div className="mbo-order__fill-bg" />
                      <span className="mbo-order__seq">{s.seq}</span>
                      <span className={`mbo-order__side mbo-order__side--${s.side}`}>
                        {s.side === 'buy' ? 'BUY' : 'SELL'}
                      </span>
                      <span className="mbo-order__qty-price">
                        {s.qty}@{s.price}
                      </span>
                      {whoLabel && (
                        <span className="mbo-order__who" style={{ color: whoColor }}>
                          {whoLabel}
                        </span>
                      )}
                      {s.isLive && <span className="mbo-order__cancel">×</span>}
                    </div>
                  )
                })
              )}
            </div>
          </div>
        )
      })}
    </div>
  )
}

// ── Main panel ────────────────────────────────────────────────────────────────
export function MboFeedPanel({ mboLogs, myOrders, onCancel, roster, playerSlot }: Props) {
  const synBook = useSyntheticBook(mboLogs, myOrders)
  return (
    <div className="mbo">
      <PriceLadder synBook={synBook} />
      <OrderFeed
        mboLogs={mboLogs}
        myOrders={myOrders}
        onCancel={onCancel}
        roster={roster}
        playerSlot={playerSlot}
      />
    </div>
  )
}

// Export synthetic best-price qty for SuitPanel "X @ Y" display.
// Reconstructs the live order book from MBO events (always up-to-date, even after
// trades that suppress book_update/book_depth on the server). Returns the SUM of
// remaining qty for all active orders at the best bid and best ask price levels,
// so the SuitPanel quick-submit button trades the correct total available quantity.
export function useMboDepth(mboLogs: Record<string, MboLogEntry[]>) {
  return useMemo(() => {
    const result: Record<string, { bidQty: number | null; askQty: number | null }> = {}
    for (const suit of SUIT_ORDER) {
      const entries = mboLogs[suit] ?? []
      // Reconstruct qty-aware book from MBO events oldest-first.
      // Track remaining qty per order: decrement on each executed fill,
      // remove when qty reaches 0 or on cancel.
      const active = new Map<number, { side: 'buy' | 'sell'; price: number; qty: number }>()
      const removed = new Set<number>()
      for (const e of entries.slice().reverse()) {
        if (removed.has(e.order_id)) continue
        if (e.kind === 'added' && e.side && e.price !== null) {
          active.set(e.order_id, { side: e.side, price: e.price, qty: e.qty ?? 1 })
        } else if (e.kind === 'executed') {
          const o = active.get(e.order_id)
          if (o) {
            o.qty -= e.qty_filled ?? 1
            if (o.qty <= 0) { active.delete(e.order_id); removed.add(e.order_id) }
          }
        } else if (e.kind === 'cancelled') {
          removed.add(e.order_id); active.delete(e.order_id)
        }
      }
      // Find best bid (highest) and best ask (lowest), summing all qty at those levels.
      let bestBid: number | null = null, bidQty = 0
      let bestAsk: number | null = null, askQty = 0
      for (const [, { side, price, qty }] of active) {
        if (side === 'buy') {
          if (bestBid === null || price > bestBid) { bestBid = price; bidQty = qty }
          else if (price === bestBid) bidQty += qty
        } else {
          if (bestAsk === null || price < bestAsk) { bestAsk = price; askQty = qty }
          else if (price === bestAsk) askQty += qty
        }
      }
      result[suit] = { bidQty: bestBid !== null ? bidQty : null, askQty: bestAsk !== null ? askQty : null }
    }
    return result
  }, [mboLogs])
}
