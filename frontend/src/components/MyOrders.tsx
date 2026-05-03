import type { MyOrder } from '../hooks/useWebSocket'
import './MyOrders.css'

interface Props {
  orders: MyOrder[]
  onCancel: (orderId: number) => void
}

// AGENT-CTX: Shows the player's resting orders as tracked by the client
// (populated from order_ack, pruned on order_cancel_ack, cleared on trade).
// Cancel is one-click — no need to type an order ID.
// Slice 8 will add a "cancel all" button and sorting by suit/side/price.
// AGENT-CTX: order_id is the key because it is server-assigned and guaranteed
// unique within a session. Do not use array index as key — the list mutates.
export function MyOrders({ orders, onCancel }: Props) {
  return (
    <div className="my-orders">
      {orders.length === 0 ? (
        <p className="my-orders__empty">No active orders.</p>
      ) : (
        <ul className="my-orders__list">
          {orders.map(o => (
            <li key={o.order_id} className="my-orders__item">
              <span className={`my-orders__side my-orders__side--${o.side}`}>
                {o.side.toUpperCase()}
              </span>
              <span className="my-orders__suit">{o.suit}</span>
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
          ))}
        </ul>
      )}
    </div>
  )
}
