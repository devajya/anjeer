// AGENT-CTX: Single source of truth for all WebSocket message shapes.
// Discriminated union on `type` — add new variants as slices introduce new events.
// Do not add UI-only data here; this file mirrors the wire protocol exactly.
// The server serialises these from C++ engine event structs in ws_server.cpp.

// ═══════════════════════════════════════════════════════════════════════════
// Server → Client (inbound)
// ═══════════════════════════════════════════════════════════════════════════

/** Sent to each client once on connect. role is absent for player connections. */
export interface PlayerHelloMessage {
  type: 'player_hello'
  player_id: number
  role?: 'player' | 'spectator'
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
  qty: number
}

/**
 * Current best bid and best ask for one suit.
 * Broadcast to all clients after every book mutation and on connect.
 * AGENT-CTX: best_bid / best_ask are null when no orders exist on that side.
 * Both fields become null simultaneously after a global wipe (any trade executes).
 * best_bid_slot / best_ask_slot are the player slot indices of the quote owners.
 */
export interface BookUpdateMessage {
  type: 'book_update'
  v: number
  seq: number
  suit: string
  best_bid: number | null
  best_ask: number | null
  best_bid_slot: number | null
  best_ask_slot: number | null
}

/**
 * A trade has executed. Broadcast to all clients.
 * your_side is personalised per recipient by the server.
 */
export interface TradeMessage {
  type: 'trade'
  suit: string
  price: number
  aggressor_side: 'buy' | 'sell'
  /** "buy" | "sell" if this client was a party to the trade; null for observers. */
  your_side: 'buy' | 'sell' | null
  buyer_slot: number
  seller_slot: number
  qty_filled: number
  qty_ordered: number
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
  starts_at?: string     // present during pre-round countdown
  round_end_at?: string  // present in mid-round spectator snapshot
  player_count: number
  usernames?: string[]   // present in mid-round spectator snapshot
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
 * roster maps slot index → username for the full DeltaTable and TradeFeed.
 */
export interface RoundStartMessage {
  type: 'round_start'
  player_slot: number
  hand: HandCounts
  round_end_at: string
  balance: number
  roster: Array<{ player_slot: number; username: string }>
  /** Total cards in hand at round start, indexed by player slot. */
  all_hand_totals: number[]
  /** Balance for every slot at round start (after buy-in), indexed by slot. */
  all_balances: number[]
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
 * Broadcast to all connected clients when a player connects or disconnects
 * before a round has started. Replaced by round_starting once countdown begins.
 *
 * AGENT-CTX: Slice 6 replaces this with lobby_joined / lobby_left events
 * scoped to a specific lobby. Until then this is a server-wide broadcast.
 * The frontend shows a Start Game button only when connected === required.
 */
export interface WaitingForStartMessage {
  type: 'waiting_for_start'
  connected: number
  required: number
}

export interface LobbyPlayer {
  player_id: number | null
  bot_uuid?: string | null
  username: string
  is_bot: boolean
  bot_difficulty?: 'easy' | 'medium' | 'hard'
  joined_at?: string
}

/** Full snapshot of a lobby, sent on subscribe_lobby and after reconnect. */
export interface LobbyStateMessage {
  type: 'lobby_state'
  lobby_id: string
  code: string
  creator_id: number
  status: 'waiting' | 'starting' | 'in_game' | 'finished' | 'closed'
  min_players: number
  max_players: number
  mode: 'ui' | 'api'
  spawn_bots_on_leave: boolean
  bot_spawn_difficulty: 'easy' | 'medium' | 'hard' | 'random'
  wipe_on_trade: boolean
  players: LobbyPlayer[]
}

/** Broadcast to all subscribers of lobby:{id} when a player joins. */
export interface PlayerJoinedMessage {
  type: 'player_joined'
  lobby_id: string
  player_id: number | null
  bot_uuid?: string | null
  username: string
  joined_at: string
  is_bot: boolean
  bot_difficulty?: 'easy' | 'medium' | 'hard'
  player_count: number
}

/** Broadcast to all subscribers of lobby:{id} when a player leaves. */
export interface PlayerLeftMessage {
  type: 'player_left'
  lobby_id: string
  player_id: number | null
  bot_uuid?: string | null
  username: string
  is_bot: boolean
  player_count: number
}

/** Client → Server: add a bot to the lobby (owner only). */
export interface AddBotMessage {
  type: 'add_bot'
  lobby_id: string
  difficulty: 'easy' | 'medium' | 'hard'
}

/** Client → Server: remove a bot from the lobby (owner only). */
export interface RemoveBotMessage {
  type: 'remove_bot'
  lobby_id: string
  bot_uuid: string
}

/**
 * Broadcast to all subscribers of lobby:{id} when the owner starts the game.
 * AGENT-CTX: On receipt, LobbyRoom navigates to /game?lobby_id=... so the
 * Game page can join the correct GameSession. The code field allows the WS
 * game server to be reached even if the client only stored lobby_id locally.
 */
export interface LobbyStartedMessage {
  type: 'lobby_started'
  lobby_id: string
  code: string
}

/**
 * Sent to all connected players after every round ends.
 * Includes results + vote state so the client can render the inter-round screen.
 * AGENT-CTX: next_round_at is null when the vote majority was already reached
 * before inter_round_seconds elapsed — the round transitions immediately.
 * Clients must handle null here and skip the countdown display.
 */
export interface InterRoundMessage {
  type: 'inter_round'
  round_number: number
  goal_suit: string
  results: PlayerRoundResult[]
  next_round_at: string | null  // ISO timestamp; null when countdown already elapsed
}

/**
 * Sent to all connected players when the game ends.
 * AGENT-CTX: net_change = final_balance − starting_balance; pre-computed
 * server-side so the client never needs to know starting_balance separately.
 */
export interface GameEndedMessage {
  type: 'game_ended'
  rounds: Array<{
    round_number: number
    goal_suit: string
    results: PlayerRoundResult[]
  }>
  final_standings: Array<{
    player_slot: number
    username: string
    final_balance: number
    net_change: number
  }>
}

/**
 * Broadcast to all connected players when a player disconnects mid-game.
 * AGENT-CTX: Distinct from PlayerLeftMessage (lobby layer) — this fires only
 * inside an active game session. The client should grey out the departed slot.
 */
export interface GamePlayerLeftMessage {
  type: 'game_player_left'
  player_slot: number
  username: string
}

/** Broadcast to all connected players when a replacement bot takes a vacated mid-round slot. */
export interface GameBotJoinedMessage {
  type: 'game_bot_joined'
  player_slot: number
  username: string
  bot_uuid: string
  bot_difficulty: 'easy' | 'medium' | 'hard'
}

/**
 * Sent to all connected players when the session encounters an unhandled error.
 * AGENT-CTX: The session tears down after emitting this. The client should
 * display a full-screen error with a "Return to lobby" button.
 * TODO(observability): extend with error_id and session_id when full stack added.
 */
export interface SessionErrorMessage {
  type: 'session_error'
  message: string
}

/**
 * Broadcast to all clients after each trade. Carries the full delta snapshot
 * for the current round so clients never need to accumulate increments.
 * AGENT-CTX: deltas[player_slot][suit_index] where suit_index matches
 * engine::kAllSuits order: 0=clubs 1=diamonds 2=hearts 3=spades.
 * Reset to all-zero at each round_start.
 */
export interface DeltaUpdateMessage {
  type: 'delta_update'
  deltas: number[][]
}

/**
 * Broadcast to all clients after every trade, after balances are settled.
 * balances[slot] = current available cash for that player slot.
 */
export interface AllBalancesMessage {
  type: 'all_balances'
  balances: number[]
}

/**
 * Broadcast to all clients after every trade, after card transfers complete.
 * totals[slot] = total cards in hand for that player slot.
 */
export interface HandTotalsMessage {
  type: 'hand_totals'
  totals: number[]
}

/** Broadcast to all players and spectators when a spectator joins or leaves. */
export interface SpectatorCountMessage {
  type: 'spectator_count'
  count: number
}

/** Forwarded script log from an API-lobby player; delivered to spectators only. */
export interface ScriptLogMessage {
  type: 'script_log'
  player_slot: number
  message: string
  timestamp: number
}

// ── Slice 11: Eval framework signals ─────────────────────────────────────────

/** One deck configuration entry inside EvalPosteriorUpdateMessage.configurations. */
export interface EvalConfiguration {
  deck_index: number
  counts: [number, number, number, number]
  goal_suit: string
  probability: number
}

/**
 * Bayesian posterior update for one player slot.
 * AGENT-CTX: target_slot in EvalOutput maps to player_slot here (private unicast).
 * The server must inject `type` and `player_slot` into the payload before sending;
 * the C++ BayesianEvalModule emits the inner fields only — see Task 20 for the
 * server-side dispatch fix that adds these wrapper fields.
 */
export interface EvalPosteriorUpdateMessage {
  type: 'eval_posterior_update'
  player_slot: number
  /** All 12 deck configurations with their posterior probabilities. */
  configurations: EvalConfiguration[]
  /** Marginal probability per goal suit (sum to 1.0). */
  goal_suit_marginals: Record<string, number>
  /** Expected payout at round end given current hand and posteriors. */
  settlement_ev: number
  /** Marginal EV gain of acquiring one additional card per suit. */
  delta_ev: Record<string, number>
}

export interface EvalPlayerSignal {
  slot: number
  player: string
  signal: 'Normal' | 'Elevated' | 'High'
  confidence: number
  primary_suit: string
}

/**
 * Per-round behavioral signal broadcast to all clients.
 * players contains one entry per active slot.
 */
export interface EvalAccumulationSignalMessage {
  type: 'eval_accumulation_signal'
  players: EvalPlayerSignal[]
}

export interface EvalSuitGuidance {
  fill_probability: number
  passive_ev: number
  trade_intensity: 'low' | 'moderate' | 'high'
  spread_width: number
  recommendation: 'passive' | 'aggressive' | 'hold'
}

/**
 * Recommended trading action + per-suit execution stats, produced by the eval pipeline.
 * action/suit/price give the single best pick; suits gives the full per-suit breakdown.
 * price is null for 'hold' guidance or when no specific price is implied.
 */
export interface EvalExecutionGuidanceMessage {
  type: 'eval_execution_guidance'
  player_slot: number
  action: 'buy' | 'sell' | 'hold'
  suit: string
  price: number | null
  suits: Record<string, EvalSuitGuidance>
}

// ── Slice 10.5: auxiliary (non-exported — snapshot-only) ─────────────────────

/** One resting order inside an order book snapshot. Not exported: snapshot use only. */
interface SnapshotOrder {
  order_id: number
  price: number
  player_slot: number
}

// ── Slice 10.5: Server → Client ──────────────────────────────────────────────

/**
 * Sent to a player immediately after their slot is assigned (at game start
 * or reattach). Client stores token in localStorage; consumed on reconnect.
 * expires_at is a Unix epoch in milliseconds.
 */
export interface ReconnectTokenMessage {
  type: 'reconnect_token'
  token: string
  expires_at: number
}

/**
 * Full session snapshot sent to a player on successful reattach.
 * AGENT-CTX: Reuses existing field shapes to avoid new client-state paths:
 *   hand        → HandCounts (suit counts, same as RoundStartMessage.hand)
 *   deltas      → number[][] (same shape as DeltaUpdateMessage.deltas)
 *   all_balances → number[] (same as RoundStartMessage.all_balances)
 *   all_scores   → number[] (same pattern, new field)
 *   roster       → same tuple as RoundStartMessage.roster
 * order_books holds full bids/asks so the hook can restore BookState and
 * reconstruct myOrders if needed; bids/asks each contain SnapshotOrder entries.
 */
export interface GameStateSnapshotMessage {
  type: 'game_state_snapshot'
  player_slot: number
  hand: HandCounts
  order_books: Record<string, { bids: SnapshotOrder[]; asks: SnapshotOrder[] }>
  deltas: number[][]
  round_timer_remaining: number
  all_balances: number[]
  all_hand_totals: number[]
  all_scores: number[]
  roster: Array<{ player_slot: number; username: string }>
  reconnect_token: string
  reconnect_expires_at: number
}

/**
 * Broadcast to all connected players when a queued player displaces a bot.
 * AGENT-CTX: new_player_id uses number (int64 within safe range); spec draft
 * used string but all other player_id fields in this file are number.
 */
export interface GameBotReplacedMessage {
  type: 'game_bot_replaced'
  slot_index: number
  bot_uuid: string
  new_player_id: number
  username: string
}

/** Sent to the joining player confirming they are in the queue. */
export interface QueueJoinedMessage {
  type: 'queue_joined'
  position: number
  queue_size: number
}

/** Ack sent to the player after a successful leave_queue command. */
export interface QueueLeftMessage {
  type: 'queue_left'
}

export type PlayersAround = { position: number; username: string; is_self: boolean }

/** Broadcast to all waiters whenever the queue mutates (join, leave, admit). */
export interface QueuePositionUpdateMessage {
  type: 'queue_position_update'
  position: number
  queue_size: number
  players_around: PlayersAround[]
}

/** Sent to a player whose join_queue request arrived when the queue was full. */
export interface QueueOverflowMessage {
  type: 'queue_overflow'
  lobby_id: string
}

/** Sent to a queued player when they are promoted into an available game slot. */
export interface QueueAdmittedMessage {
  type: 'queue_admitted'
  slot_index: number
  lobby_id: string
}

/**
 * Sent to a disconnected client when the reconnect window expires server-side.
 * AGENT-CTX: On receipt the client clears localStorage token, shows expiry
 * message, and offers queue-join or spectate options.
 */
export interface ReconnectWindowExpiredMessage {
  type: 'reconnect_window_expired'
}

/**
 * Broadcast when lobby ownership transfers (owner leaves/expires) or unicast
 * to a player on slot attach so they always know the current owner.
 * AGENT-CTX: The owner controls inter-round progression (start_next_round).
 * Non-owners see a "Waiting for owner" message instead of the Start button.
 */
export interface LobbyOwnerChangedMessage {
  type: 'lobby_owner_changed'
  new_owner_player_id: number
  new_owner_username: string
}

/**
 * Broadcast to all lobby subscribers when the owner changes bot-autofill settings
 * via PATCH /lobbies/{id}/bot-settings. Non-creator players update their toggle
 * display from this message rather than waiting for a full lobby_state re-fetch.
 */
export interface LobbySettingsChangedMessage {
  type: 'lobby_settings_changed'
  lobby_id: string
  spawn_bots_on_leave: boolean
  bot_spawn_difficulty: 'easy' | 'medium' | 'hard' | 'random'
  wipe_on_trade?: boolean
}

// ── Slice 14: Market data feed tiers ─────────────────────────────────────────

/** MBP-N incremental: full depth snapshot per suit after any order event. */
export interface BookDepthMessage {
  type: 'book_depth'
  v: number
  seq: number
  suit: string
  bids: { price: number; qty: number }[]
  asks: { price: number; qty: number }[]
}

/** MBP-N snapshot: sent on connect or resync for each instrument. */
export interface BookDepthSnapshotMessage {
  type: 'book_depth_snapshot'
  v: number
  seq: number
  suit: string
  bids: { price: number; qty: number }[]
  asks: { price: number; qty: number }[]
}

/** MBO incremental: a new resting order entered the book. */
export interface OrderAddedMessage {
  type: 'order_added'
  v: number
  seq: number
  order_id: number
  suit: string
  side: 'buy' | 'sell'
  price: number
}

/**
 * MBO incremental: an order was filled.
 * AGENT-CTX: Use order_id to remove the resting order from a local MBO book.
 * buyer_slot / seller_slot identify the counterparties for display purposes.
 */
export interface OrderExecutedMessage {
  type: 'order_executed'
  v: number
  seq: number
  order_id: number
  suit: string
  price: number
  aggressor_side: 'buy' | 'sell'
  buyer_slot: number
  seller_slot: number
}

/** MBO incremental: a resting order was cancelled. */
export interface OrderCancelledMessage {
  type: 'order_cancelled'
  v: number
  seq: number
  order_id: number
  suit: string
}

/**
 * MBO snapshot: full order-by-order book state per suit.
 * Sent on connect and on resync for MBO connections (including /ws/marketdata).
 * AGENT-CTX: seq equals the last applied exchange event; next incremental will be seq+1.
 */
export interface OrderBookSnapshotMessage {
  type: 'order_book_snapshot'
  v: number
  seq: number
  suit: string
  bids: { order_id: number; price: number }[]
  asks: { order_id: number; price: number }[]
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
  | WaitingForStartMessage
  | LobbyStateMessage
  | PlayerJoinedMessage
  | PlayerLeftMessage
  | LobbyStartedMessage
  | InterRoundMessage
  | GameEndedMessage
  | GamePlayerLeftMessage
  | GameBotJoinedMessage
  | SessionErrorMessage
  | DeltaUpdateMessage
  | AllBalancesMessage
  | HandTotalsMessage
  | SpectatorCountMessage
  | ScriptLogMessage
  // Slice 10.5
  | ReconnectTokenMessage
  | GameStateSnapshotMessage
  | GameBotReplacedMessage
  | QueueJoinedMessage
  | QueueLeftMessage
  | QueuePositionUpdateMessage
  | QueueOverflowMessage
  | QueueAdmittedMessage
  | ReconnectWindowExpiredMessage
  | LobbyOwnerChangedMessage
  | LobbySettingsChangedMessage
  // Slice 11
  | EvalPosteriorUpdateMessage
  | EvalAccumulationSignalMessage
  | EvalExecutionGuidanceMessage
  // Slice 14
  | BookDepthMessage
  | BookDepthSnapshotMessage
  | OrderAddedMessage
  | OrderExecutedMessage
  | OrderCancelledMessage
  | OrderBookSnapshotMessage

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
  qty: number
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

/**
 * Sent by any connected client to trigger the game countdown.
 * Server accepts this only when all slots are filled and no countdown is active.
 * AGENT-CTX: No auth on who can trigger this in Slice 5 — any connected player
 * can start. Slice 6 adds lobby owner enforcement via lobby REST API.
 */
export interface StartGameCommand {
  type: 'start_game'
}

/** Subscribe to real-time lobby events (player_joined, player_left, lobby_started). */
export interface SubscribeLobbyCommand {
  type: 'subscribe_lobby'
  lobby_id: string
}

/** Stop receiving lobby events for this connection. */
export interface UnsubscribeLobbyCommand {
  type: 'unsubscribe_lobby'
  lobby_id: string
}

/**
 * Sent by the client before back-navigating out of a lobby room.
 * AGENT-CTX: Required so WsServer can call LobbyRepo::remove_player and
 * delete_if_empty atomically. Without this the player row lingers until
 * the WebSocket closes, causing a stale lobby_players row.
 */
export interface LeaveLobbyCommand {
  type: 'leave_lobby'
  lobby_id: string
}

/** Sent once after WS connect to enter a session as a read-only spectator.
 *  Prefer lobby_code (human-readable); lobby_id (UUID) accepted for backwards compat. */
export interface SpectateLobbyCommand {
  type: 'spectate_lobby'
  lobby_code?: string
  lobby_id?: string
}

/** API-lobby players only. Forwarded to spectators as ScriptLogMessage. */
export interface ScriptLogCommand {
  type: 'script_log'
  message: string
}

// Slice 10.5 client commands

/**
 * Sent by the session owner during the inter-round window to start the next
 * round immediately, bypassing the countdown. Server silently drops this if
 * the sender is not the current owner.
 */
export interface StartNextRoundCommand {
  type: 'start_next_round'
}

/** Sent by the session owner during inter-round to end the game immediately. */
export interface EndGameCommand {
  type: 'end_game'
}

/** Enqueue for an active lobby that has no open slots. */
export interface JoinQueueCommand {
  type: 'join_queue'
  lobby_id: string
}

/** Leave the wait queue for a lobby. */
export interface LeaveQueueCommand {
  type: 'leave_queue'
  lobby_id: string
}

/**
 * In-session reattach using a stored reconnect token.
 * Also handled at upgrade time via ?token= query param.
 */
export interface ReconnectGameCommand {
  type: 'reconnect_game'
  lobby_id: string
  token: string
}

/**
 * Sent by a connected client to request a full snapshot for their current feed tier.
 * MBP-1 → one book_update per instrument; MBP-N → book_depth_snapshot × 4;
 * MBO → order_book_snapshot × 4.
 */
export interface ResyncCommand {
  type: 'resync'
}

export type ClientCommand =
  | SubmitOrderCommand
  | NudgeCommand
  | CancelOrderCommand
  | StartGameCommand
  | SubscribeLobbyCommand
  | UnsubscribeLobbyCommand
  | LeaveLobbyCommand
  | SpectateLobbyCommand
  | ScriptLogCommand
  | AddBotMessage
  | RemoveBotMessage
  // Slice 10.5
  | JoinQueueCommand
  | LeaveQueueCommand
  | ReconnectGameCommand
  | StartNextRoundCommand
  | EndGameCommand
  // Slice 14
  | ResyncCommand

// ═══════════════════════════════════════════════════════════════════════════
// HTTP REST types (not WebSocket)
// ═══════════════════════════════════════════════════════════════════════════

/** One row from GET /players/me/api-keys. key_hash is never returned. */
export interface ApiKeyView {
  id:         number
  name:       string
  created_at: string   // ISO8601
  expires_at: string   // ISO8601
  revoked_at: string | null
}

/** 201 response body from POST /players/me/api-keys. key shown exactly once. */
export interface ApiKeyCreateResponse {
  id:         number
  key:        string   // plaintext "ank_<64hex>" — store nowhere, show once
  name:       string
  expires_at: string   // ISO8601
}
