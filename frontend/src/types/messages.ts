// AGENT-CTX: Single source of truth for all WebSocket message shapes.
// Discriminated union on `type` — add new variants as slices introduce new events.
// Do not add UI-only data here; this file mirrors the wire protocol exactly.
// The server serialises these from C++ engine event structs in ws_server.cpp.

// ═══════════════════════════════════════════════════════════════════════════
// Server → Client (inbound)
// ═══════════════════════════════════════════════════════════════════════════

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
 *   UNKNOWN_SUIT | MALFORMED_MESSAGE | INSUFFICIENT_BALANCE
 */
export interface ErrorMessage {
  type: 'error'
  code: string
  message: string
}

/**
 * Broadcast to ALL players when enough connections have joined.
 * starts_at is an ISO 8601 UTC timestamp — clients drive their own countdown.
 * AGENT-CTX: One message, client-side timer. Avoids four separate tick events.
 * Evolves into lobby_started (with player roster) in the lobby slice.
 */
export interface RoundStartingMessage {
  type: 'round_starting'
  starts_at: string   // ISO 8601 UTC, e.g. "2026-04-14T18:03:03.000Z"
  player_count: number
}

/** Per-suit card counts in a player's hand. Reused by RoundStartMessage and HandPanel. */
export interface HandCounts {
  clubs: number
  diamonds: number
  hearts: number
  spades: number
}

/**
 * Sent to each player individually after countdown expires.
 * Contains only that player's own hand counts — no other player's info,
 * no suit totals, no goal suit.
 * AGENT-CTX: goal_suit and suit_totals are absent by design (Slice 3
 * resolution 6 — spec error corrected: suit counts are private).
 */
export interface RoundStartMessage {
  type: 'round_start'
  player_slot: number
  hand: HandCounts
  round_end_at: string
  balance: number
}

/** Per-player entry in round_end standings. */
export interface PlayerRoundResult {
  player_slot: number
  goal_cards_held: number
  payout: number
  /** available_cash after payout is applied — server-owned state, not engine-computed. */
  balance: number
  disconnected: boolean
}

/**
 * Sent to buyer and seller after each executed trade.
 * Reflects available_cash after the price has been debited/credited.
 * AGENT-CTX: Single source of balance truth mid-round; replaces the round_start
 * value. When Slice 8 adds server-side balance persistence, only the server
 * write path changes — this message shape stays the same.
 */
export interface BalanceUpdateMessage {
  type: 'balance_update'
  balance: number
}

/**
 * Sent privately to each connected player at round end.
 * Includes all players (including disconnected) so the client can render
 * a full standings table. Own slot is identifiable by player_slot.
 */
export interface RoundEndMessage {
  type: 'round_end'
  goal_suit: string
  results: PlayerRoundResult[]
}

/**
 * ServerMessage is the exhaustive union of all server-to-client message types.
 * AGENT-CTX: Every new server event type must be added here. The switch in
 * useWebSocket.ts is exhaustive — TypeScript will error on unhandled variants
 * once the union has more than one member (the `never` check at the bottom).
 */
export type ServerMessage =
  | PlayerHelloMessage
  | OrderAckMessage
  | BookUpdateMessage
  | TradeMessage
  | OrderCancelAckMessage
  | ErrorMessage
  | RoundStartingMessage
  | RoundStartMessage
  | RoundEndMessage
  | BalanceUpdateMessage

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
