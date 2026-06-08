import type { MyOrder } from '../hooks/useWebSocket'
import { useGameConfig } from '../contexts/GameConfigContext'
import './MyOrders.css'

const SUIT_SYMBOLS: Record<string, string> = { clubs: '♣', diamonds: '♦', hearts: '♥', spades: '♠' }

interface Props {
  orders: MyOrder[]
  onCancel: (orderId: number) => void
}

export function MyOrders({ orders, onCancel }: Props) {
  const { allowMultiQty } = useGameConfig()

  return (
    <div className="my-orders">
      {orders.length === 0 ? (
        <p className="my-orders__empty">No active orders.</p>
      ) : (
        <ul className="my-orders__list">
          {orders.map(o => {
            const fillPct = o.qty > 0 ? ((o.qty - o.qty_remaining) / o.qty) * 100 : 0
            return (
            <li
              key={o.order_id}
              className="my-orders__item"
              style={{ '--fill-pct': `${fillPct}%` } as React.CSSProperties}
            >
              <div className="my-orders__fill-bg" />
              <span className="my-orders__suit-sym">{SUIT_SYMBOLS[o.suit] ?? o.suit}</span>
              <span className={`my-orders__side my-orders__side--${o.side}`}>
                {o.side.toUpperCase()}
              </span>
              {allowMultiQty ? (
                <span className="my-orders__qty">
                  {o.qty_remaining < o.qty
                    ? `${o.qty_remaining}/${o.qty}`
                    : String(o.qty)}
                </span>
              ) : null}
              <span className="my-orders__price">@ {o.price}</span>
              <span className="my-orders__id">#{o.order_id}</span>
              <button
                className="my-orders__cancel"
                type="button"
                onClick={() => onCancel(o.order_id)}
                aria-label={`Cancel order ${o.order_id}`}
              >
                ✕
              </button>
            </li>
            )
          })}
        </ul>
      )}
    </div>
  )
}
