import { useMemo, useRef, useEffect } from 'react'
import type { MboLogEntry, MyOrder } from '../hooks/useWebSocket'
import { SUIT_SYMBOLS, suitClass } from '../utils/suits'
import './MboFeedPanel.css'

const SUIT_ORDER = ['clubs', 'diamonds', 'hearts', 'spades'] as const

interface BookState { best_bid: number | null; best_ask: number | null }

interface Props {
  mboLogs:  Record<string, MboLogEntry[]>
  myOrders: MyOrder[]
  books:    Record<string, BookState>
  onCancel: (orderId: number) => void
}

// ── Synthetic book derived from MBO event log ─────────────────────────────────
// Each suit yields sorted bid/ask price levels and a flat resting-order list.
// Qty is approximate (1 per order) because order_added carries no qty field.
interface RestingOrder { order_id: number; side: 'buy' | 'sell'; price: number; isMine: boolean }
interface SyntheticSuit {
  bids: { price: number; count: number }[]  // highest-first
  asks: { price: number; count: number }[]  // lowest-first
  all:  RestingOrder[]                       // all resting, sorted price desc
}

function useSyntheticBook(
  mboLogs:  Record<string, MboLogEntry[]>,
  myOrders: MyOrder[],
): Record<string, SyntheticSuit> {
  const myIds = useMemo(() => new Set(myOrders.map(o => o.order_id)), [myOrders])
  return useMemo(() => {
    const out: Record<string, SyntheticSuit> = {}
    for (const suit of SUIT_ORDER) {
      const entries  = mboLogs[suit] ?? []
      const active   = new Map<number, { side: 'buy' | 'sell'; price: number }>()
      const removed  = new Set<number>()
      // Process oldest-first (log is newest-first)
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
      const all: RestingOrder[] = []
      for (const [order_id, { side, price }] of active) {
        const m = side === 'buy' ? bidMap : askMap
        m.set(price, (m.get(price) ?? 0) + 1)
        all.push({ order_id, side, price, isMine: myIds.has(order_id) })
      }
      out[suit] = {
        bids: [...bidMap].map(([price, count]) => ({ price, count })).sort((a, b) => b.price - a.price),
        asks: [...askMap].map(([price, count]) => ({ price, count })).sort((a, b) => a.price - b.price),
        all:  all.sort((a, b) => b.price - a.price),
      }
    }
    return out
  }, [mboLogs, myIds])
}

// ── Variant 1: Price Ladder ───────────────────────────────────────────────────
// Bid/ask levels derived from MBO events, one row per price level per suit.
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
                    <div
                      className="mbo-vis__bar mbo-vis__bar--bid"
                      style={{ width: `${(lvl.count / maxCount) * 100}%` }}
                    />
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
                    <div
                      className="mbo-vis__bar mbo-vis__bar--ask"
                      style={{ width: `${(lvl.count / maxCount) * 100}%` }}
                    />
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


// ── Event log ─────────────────────────────────────────────────────────────────
function EventLog({
  mboLogs,
  myOrders,
  onCancel,
}: {
  mboLogs:  Record<string, MboLogEntry[]>
  myOrders: MyOrder[]
  onCancel: (orderId: number) => void
}) {
  // Accumulate all order IDs that ever appeared in myOrders so we can
  // identify executed/cancelled events that belong to us.
  const allMineIds = useRef(new Set<number>())
  useEffect(() => {
    myOrders.forEach(o => allMineIds.current.add(o.order_id))
  }, [myOrders])

  const liveIds = useMemo(() => new Set(myOrders.map(o => o.order_id)), [myOrders])

  return (
    <div className="mbo-log">
      {SUIT_ORDER.map(suit => {
        const entries = mboLogs[suit] ?? []
        return (
          <div key={suit} className="mbo-log__section">
            <div className="mbo-log__hdr">
              <span className={`mbo-log__symbol ${suitClass(suit)}`}>{SUIT_SYMBOLS[suit]}</span>
              <span className="mbo-log__name">{suit}</span>
            </div>
            <div className="mbo-log__feed">
              {entries.length === 0 ? (
                <div className="mbo-log__empty">no events</div>
              ) : (
                entries.map((e, i) => {
                  const isMineActive    = e.kind === 'added'     && liveIds.has(e.order_id)
                  const isMineExecuted  = e.kind === 'executed'  && allMineIds.current.has(e.order_id)
                  const iMineCancelled  = e.kind === 'cancelled' && allMineIds.current.has(e.order_id)
                  const isMine = isMineActive || isMineExecuted || iMineCancelled
                  return (
                    <div
                      key={i}
                      className={[
                        'mbo-log__row',
                        `mbo-log__row--${e.kind}`,
                        isMineActive   ? 'mbo-log__row--mine'            : '',
                        isMineExecuted ? 'mbo-log__row--mine-done'       : '',
                        iMineCancelled ? 'mbo-log__row--mine-cancelled'  : '',
                      ].filter(Boolean).join(' ')}
                      onClick={isMineActive ? () => onCancel(e.order_id) : undefined}
                      title={isMineActive ? 'Tap to cancel' : undefined}
                    >
                      <span className="mbo-log__seq">{e.seq}</span>
                      <span className="mbo-log__icon">
                        {e.kind === 'added' ? '+' : e.kind === 'executed' ? '✓' : '–'}
                      </span>
                      <span className="mbo-log__id">#{e.order_id}</span>
                      {e.kind === 'added' && e.side && (
                        <span className={`mbo-log__side mbo-log__side--${e.side}`}>
                          {e.side.toUpperCase()}
                        </span>
                      )}
                      {e.price !== null && (
                        <span className="mbo-log__price">{e.price}</span>
                      )}
                      {isMine && <span className="mbo-log__you">you</span>}
                      {isMineActive && <span className="mbo-log__cancel">×</span>}
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
export function MboFeedPanel({ mboLogs, myOrders, books: _books, onCancel }: Props) {
  const synBook = useSyntheticBook(mboLogs, myOrders)
  return (
    <div className="mbo">
      <PriceLadder synBook={synBook} />
      <EventLog mboLogs={mboLogs} myOrders={myOrders} onCancel={onCancel} />
    </div>
  )
}
