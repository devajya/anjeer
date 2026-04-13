// AGENT-CTX: Single source of truth for all WebSocket message shapes.
// Discriminated union on `type` — add new variants as slices introduce new events.
// Do not add UI-only data here; this file mirrors the wire protocol exactly.
// The server serialises these from C++ engine event structs in ws_server.cpp.

// ═══════════════════════════════════════════════════════════════════════════
// Server → Client (inbound)
// ═══════════════════════════════════════════════════════════════════════════

export interface HeartbeatMessage {
  type: 'heartbeat'
  /** Unix timestamp in milliseconds from the server clock. */
  server_ts: number
}

/** Sent to each client once on connect. Transient identity until Slice 5 auth. */
export interface PlayerHelloMessage {
  type: 'player_hello'
  player_id: number
}

/**
 * Confirms a successfully placed or nudged order.
 * Sent only to the submitting client.
 * AGENT-CTX: Store order_id — it is required to cancel this order later.
 */
export interface OrderAckMessage {
  type: 'order_ack'
  order_id: number
  suit: string
  side: 'buy' | 'sell'
  price: number
}

/**
 * Current best bid and best ask for one suit.
 * Broadcast to all clients after every book mutation and on connect.
 * AGENT-CTX: best_bid / best_ask are null when no orders exist on that side.
 * Both fields become null simultaneously after a global wipe (any trade executes).
 */
export interface BookUpdateMessage {
  type: 'book_update'
  suit: string
  best_bid: number | null
  best_ask: number | null
}

/**
 * A trade has executed. Broadcast to all clients.
 * your_side is personalised per recipient by the server.
 * AGENT-CTX: qty is absent — every trade is for exactly 1 card (Slice 2 mechanic).
 * When multi-card orders are added, insert qty: number here and update the display.
 */
export interface TradeMessage {
  type: 'trade'
  suit: string
  price: number
  aggressor_side: 'buy' | 'sell'
  /** "buy" | "sell" if this client was a party to the trade; null for observers. */
  your_side: 'buy' | 'sell' | null
}

/** Confirms a successfully cancelled order. Sent only to the cancelling client. */
export interface OrderCancelAckMessage {
  type: 'order_cancel_ack'
  order_id: number
}

/**
 * A typed error from the server. Sent only to the relevant client.
 * AGENT-CTX: Switch on `code`, not `message`. message is human-readable and
 * may change between releases. Stable codes (from server error_code_str()):
 *   PRICE_OUT_OF_RANGE | ORDER_NOT_FOUND | NOT_YOUR_ORDER |
 *   UNKNOWN_SUIT | MALFORMED_MESSAGE
 */
export interface ErrorMessage {
  type: 'error'
  code: string
  message: string
}

/**
 * ServerMessage is the exhaustive union of all server-to-client message types.
 * AGENT-CTX: Every new server event type must be added here. The switch in
 * useWebSocket.ts is exhaustive — TypeScript will error on unhandled variants
 * once the union has more than one member (the `never` check at the bottom).
 */
export type ServerMessage =
  | HeartbeatMessage
  | PlayerHelloMessage
  | OrderAckMessage
  | BookUpdateMessage
  | TradeMessage
  | OrderCancelAckMessage
  | ErrorMessage

// ═══════════════════════════════════════════════════════════════════════════
// Client → Server (outbound commands)
// These types document the wire format for callers of sendMessage() and serve
// as a compile-time reference. The server validates all fields independently.
// ═══════════════════════════════════════════════════════════════════════════

export interface SubmitOrderCommand {
  type: 'submit_order'
  suit: string
  side: 'buy' | 'sell'
  price: number
}

export interface NudgeCommand {
  type: 'nudge'
  suit: string
  side: 'buy' | 'sell'
}

export interface CancelOrderCommand {
  type: 'cancel_order'
  order_id: number
}

export type ClientCommand = SubmitOrderCommand | NudgeCommand | CancelOrderCommand
