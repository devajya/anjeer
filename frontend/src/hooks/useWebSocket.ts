import { useState, useEffect, useRef, useCallback } from 'react'
import { decode } from '@msgpack/msgpack'
import type {
  ServerMessage,
  ErrorMessage,
  RoundEndMessage,
  ClientCommand,
  HandCounts,
  WaitingForStartMessage,
  LobbyStateMessage,
  LobbyStartedMessage,
  InterRoundMessage,
  GameEndedMessage,
  SessionErrorMessage,
  GameStateSnapshotMessage,
  PlayersAround,
} from '../types/messages'
import { logger } from '../logger'
import { applyMessage } from './useWsReducer'

// ─── Domain types exposed by the hook ────────────────────────────────────────

export type QueueState =
  | { status: 'idle' }
  | { status: 'queued'; lobbyId: string; position: number; queueSize: number; playersAround: PlayersAround[] }
  | { status: 'overflow'; lobbyId: string }
  | { status: 'admitted'; lobbyId: string; slotIndex: number }

export interface BookState {
  best_bid: number | null
  best_ask: number | null
  best_bid_slot: number | null
  best_ask_slot: number | null
}

// Cleared on trade in simple/intermediate (global wipe); preserved in advanced (no-wipe) mode.
export interface MyOrder {
  order_id:      number
  suit:          string
  side:          'buy' | 'sell'
  price:         number
  qty:           number
  qty_remaining: number
}

/**
 * A single entry in the trade history feed.
 * `id` is a local monotonic counter used as a React key — it is NOT the
 * server-side order ID and has no game meaning.
 */
export interface TradeEntry {
  id: number
  suit: string
  price: number
  aggressor_side: 'buy' | 'sell'
  your_side: 'buy' | 'sell' | null
  buyer_slot: number
  seller_slot: number
  qty_filled: number
  qty_ordered: number
  ts: number  // Date.now() at receipt — used for display only
}

export interface WsState {
  connected: boolean
  /** Transient player identity assigned by the server on connect. null until received. */
  playerId: number | null
  /**
   * Current best bid/ask per active suit, keyed by suit name (e.g. "S1").
   * Populated as book_update messages arrive. Empty on first render.
   */
  books: Record<string, BookState>
  /**
   * Most recent trades, newest first. Capped at MAX_TRADE_HISTORY entries.
   */
  trades: TradeEntry[]
  /**
   * Last error per suit, keyed by suit name. Only suit-scoped operations
   * (submit_order, nudge) populate this; non-suit errors (e.g. cancel) are
   * stored under '_' and are not shown in SuitPanel.
   * Cleared per-suit on the next successful order_ack for that suit.
   */
  errors: Record<string, ErrorMessage | null>
  /**
   * Client-side list of this player's resting orders (not yet filled or cancelled).
   * Populated from order_ack; pruned on order_cancel_ack and cleared on any trade
   * (because a trade triggers a global book wipe in Slice 2).
   */
  myOrders: MyOrder[]
  /** ISO 8601 UTC timestamp from round_starting. null when no countdown is active. */
  startsAt: string | null
  /** This player's hand counts, updated in real-time as trades execute. null until round_start. */
  hand: HandCounts | null
  /**
   * Hand counts as-dealt at round_start. Held constant through the round so
   * HandPanel can show per-suit deltas (current − initial).
   */
  initialHand: HandCounts | null
  /** This player's slot index (0-indexed). null until round_start is received. */
  playerSlot: number | null
  /** ISO 8601 UTC deadline for the active round. null until round_start; cleared on round_end. */
  roundEndAt: string | null
  /** Last round_end payload. null until first round completes. */
  roundEnd: RoundEndMessage | null
  /** Available cash. Updated on round_start (after buy-in deducted), all_balances (after trade), and round_end (after payout). null until first round_start. */
  balance: number | null
  /**
   * Current lobby fill state. Non-null while phase == Waiting (before countdown).
   * Null once round_starting arrives. Frontend shows Start Game button when
   * connected === required.
   */
  waitingForStart: WaitingForStartMessage | null
  /**
   * Full lobby snapshot. Set on lobby_state; players array is updated
   * incrementally by player_joined / player_left without a full re-fetch.
   */
  lobbyState: LobbyStateMessage | null
  /**
   * Set when lobby_started arrives. LobbyRoom watches this in a useEffect
   * and navigates to /game?lobby_id=... when lobby_id matches current lobby.
   * Never cleared by the hook — the lobby_id guard in LobbyRoom prevents
   * spurious re-navigation on reconnect.
   */
  lobbyStarted: LobbyStartedMessage | null
  /**
   * Set on inter_round; cleared when a new round_start arrives.
   */
  interRound: InterRoundMessage | null
  /** Set on game_ended; never cleared — the session is over at that point. */
  gameEnded: GameEndedMessage | null
  /** Set on session_error; never cleared — the session is over at that point. */
  sessionError: SessionErrorMessage | null
  /**
   * Slot indices of players who disconnected mid-game (game_player_left).
   * Accumulates so the UI keeps departed players greyed out after multiple departures.
   */
  departedSlots: number[]
  /**
   * Full delta snapshot for the current round. deltas[player_slot][suit_index]
   * where suit_index: 0=clubs 1=diamonds 2=hearts 3=spades.
   * Reset to empty array on each round_start; updated by delta_update.
   */
  deltas: number[][]
  /**
   * Slot→username mapping for the current game. Populated from roster field
   * in round_start; stable for the round duration.
   */
  roster: Array<{ player_slot: number; username: string }>
  /** Current total cards per slot. Set at round_start; updated after each trade. */
  allHandTotals: number[]
  /** Current balance per slot. Set at round_start; updated after each trade. */
  allBalances: number[]
  /** Live count of spectators watching this session. Updated by spectator_count events. */
  spectatorCount: number
  /** Script log entries from API-lobby players. Delivered to spectators only. */
  scriptLogs: (import('../types/messages').ScriptLogMessage & { _seq: number })[]
  // ── Slice 10.5 reconnect signals ─────────────────────────────────────────
  // Write-once — never cleared by the hook because clearing would require a
  // second state flush and risks a missed event on fast successive messages.
  /** Latest reconnect_token message from server. null until first token issued. */
  reconnectTokenMsg: { token: string; expires_at: number } | null
  /** Full state snapshot from server on slot reattach. null until first reattach. */
  gameStateSnapshot: GameStateSnapshotMessage | null
  /** True once reconnect_window_expired is received (terminal; never cleared). */
  reconnectWindowExpired: boolean
  /** Queue membership state. Tracks position updates and terminal signals (overflow, admitted). */
  queueState: QueueState
  /**
   * player_id of the current session owner. Set on lobby_owner_changed (unicast
   * on attach, broadcast on transfer). null until first message received.
   */
  currentOwnerPlayerId: number | null
  /** Username of the current session owner. Set alongside currentOwnerPlayerId. */
  currentOwnerUsername: string
  // ── Slice 11: Eval signals ────────────────────────────────────────────────
  /**
   * Latest Bayesian posterior update for this player slot. null until the first
   * eval_posterior_update message arrives. Overwritten on each message — the eval
   * pipeline sends full snapshots, not incremental deltas.
   */
  evalPosteriorUpdate: import('../types/messages').EvalPosteriorUpdateMessage | null
  evalAccumulationSignal: import('../types/messages').EvalAccumulationSignalMessage | null
  evalExecutionGuidance: import('../types/messages').EvalExecutionGuidanceMessage | null
  // ── Slice 15: game mode + market data ────────────────────────────────────
  /** Game mode for the active round. Populated from round_start.game_mode. */
  gameMode: import('../types/messages').GameMode | null
  /** Top-of-book depth per suit. Populated by book_depth / book_depth_snapshot messages. */
  bookDepths: Record<string, { bids: { price: number; qty: number }[]; asks: { price: number; qty: number }[] }>
  /** Per-suit MBO event log, newest first, capped at 100. */
  mboLogs: Record<string, MboLogEntry[]>
}

export interface MboLogEntry {
  kind:               'added' | 'executed' | 'cancelled'
  seq:                number
  order_id:           number
  price:              number | null
  side?:              'buy' | 'sell'
  owner_slot?:        number    // 'added' only
  qty?:               number    // 'added' only — original order qty
  qty_filled?:        number    // 'executed' only
  aggressor_order_id?: number   // 'executed' only — the crossing order's id
  buyer_slot?:        number    // 'executed' only
  seller_slot?:       number    // 'executed' only
}

// ─── Hook ─────────────────────────────────────────────────────────────────────

// url must be a relative path (e.g. '/ws') — the hook builds the absolute ws(s):// URL
// from window.location so the same code works behind Vite's proxy and nginx.
export type UseWebSocketReturn = WsState & {
  sendMessage:        (cmd: ClientCommand) => void
  ownsBestBidBySuit:  Record<string, boolean>
  ownsBestAskBySuit:  Record<string, boolean>
  /** Sends subscribe_lobby over the open WS. No-op if socket not open. */
  subscribeLobby:     (lobby_id: string) => void
  /** Sends unsubscribe_lobby. Called by LobbyRoom cleanup effect on unmount. */
  unsubscribeLobby:   (lobby_id: string) => void
  /** Enqueue for an active lobby. Tracks lobbyId so queue messages can populate queueState. */
  joinQueue:          (lobby_id: string) => void
  /** Leave the current queue. No-op if not queued. */
  leaveQueue:         (lobby_id: string) => void
  /** Reset queue state to idle (e.g. after navigation away). */
  resetQueue:         () => void
}

export function useWebSocket(url: string): UseWebSocketReturn {
  const [state, setState] = useState<WsState>({
    connected: false,
    playerId: null,
    books: {},
    trades: [],
    errors: {},
    myOrders: [],
    startsAt: null,
    hand: null,
    initialHand: null,
    playerSlot: null,
    roundEndAt: null,
    roundEnd: null,
    balance: null,
    waitingForStart: null,
    lobbyState: null,
    lobbyStarted: null,
    interRound: null,
    gameEnded: null,
    sessionError: null,
    departedSlots: [],
    deltas: [],
    roster: [],
    allHandTotals: [],
    allBalances: [],
    spectatorCount: 0,
    scriptLogs: [],
    reconnectTokenMsg: null,
    gameStateSnapshot: null,
    reconnectWindowExpired: false,
    queueState: { status: 'idle' },
    currentOwnerPlayerId: null,
    currentOwnerUsername: '',
    evalPosteriorUpdate: null,
    evalAccumulationSignal: null,
    evalExecutionGuidance: null,
    gameMode: null,
    bookDepths: {},
    mboLogs: {},
  })

  const wsRef = useRef<WebSocket | null>(null)
  // Local counter for TradeEntry.id — never reset, guarantees unique React keys.
  const tradeSeqRef = useRef(0)
  // Tracks which suit the most-recently sent command targeted, so that an
  // incoming error message can be keyed to the correct SuitPanel. Null for
  // commands with no suit (e.g. cancel_order), which store errors under '_'.
  const pendingSuitRef = useRef<string | null>(null)
  // Tracks the lobby_id of the most-recently sent join_queue, so queue_joined /
  // queue_position_update (which carry no lobby_id) can populate queueState.lobbyId.
  const pendingQueueLobbyRef = useRef<string | null>(null)

  useEffect(() => {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
    const fullUrl = `${protocol}//${window.location.host}${url}`
    logger.info('ws', `connecting to ${fullUrl}`)

    const ws = new WebSocket(fullUrl, 'anjeer-msgpack')
    ws.binaryType = 'arraybuffer'
    wsRef.current = ws

    ws.onopen = () => {
      logger.info('ws', 'connection opened')
      setState(s => ({ ...s, connected: true, errors: {} }))
    }

    ws.onclose = (ev) => {
      logger.warn('ws', `connection closed — code=${ev.code} reason="${ev.reason}" wasClean=${ev.wasClean}`)
      setState(s => ({ ...s, connected: false }))
      // TODO(Slice 5+): no reconnect attempted. When added, re-sync slot state
      // (hand, active orders) on reconnect — the server will need to replay them.
      // Guard: React 18 StrictMode remounts in dev — only clear if this ws is still current.
      if (wsRef.current === ws) {
        wsRef.current = null
      }
    }

    ws.onerror = (ev) => {
      logger.error('ws', 'WebSocket error event', ev)
      // Do not clear wsRef here — onclose always fires after onerror.
      // Clearing here would race with the remounted socket in StrictMode.
    }

    ws.onmessage = (event: MessageEvent) => {
      let msg: ServerMessage
      try {
        if (event.data instanceof ArrayBuffer) {
          msg = decode(event.data) as ServerMessage
        } else {
          msg = JSON.parse(event.data as string) as ServerMessage
        }
      } catch (err) {
        logger.error('ws', 'failed to parse server message', { raw: event.data, err })
        return
      }

      // Exhaustive switch — the `default` never-check surfaces unhandled ServerMessage variants at compile time.
      switch (msg.type) {
        case 'player_hello':
          logger.info('ws/recv', `player_hello player_id=${msg.player_id}`)
          setState(s => ({ ...s, playerId: msg.player_id }))
          break

        case 'order_ack':
          logger.info('ws/recv', `order_ack order_id=${msg.order_id} suit=${msg.suit} side=${msg.side} price=${msg.price} qty=${msg.qty}`)
          setState(s => ({
            ...s,
            errors: { ...s.errors, [msg.suit]: null },
            myOrders: [
              ...s.myOrders,
              { order_id: msg.order_id, suit: msg.suit, side: msg.side, price: msg.price, qty: msg.qty, qty_remaining: msg.qty },
            ],
          }))
          break

        case 'book_update': {
          logger.info('ws/recv', `book_update suit=${msg.suit} best_bid=${msg.best_bid} best_ask=${msg.best_ask}`)
          setState(s => applyMessage(s, msg, tradeSeqRef))
          break
        }

        case 'trade': {
          logger.info('ws/recv', `trade suit=${msg.suit} price=${msg.price} aggressor=${msg.aggressor_side} your_side=${msg.your_side}`)
          setState(s => applyMessage(s, msg, tradeSeqRef))
          break
        }

        case 'order_cancel_ack': {
          logger.info('ws/recv', `order_cancel_ack order_id=${msg.order_id}`)
          setState(s => ({
            ...s,
            myOrders: s.myOrders.filter(o => o.order_id !== msg.order_id),
          }))
          break
        }

        case 'error': {
          logger.error('ws/recv', `error code=${msg.code} message="${msg.message}"`)
          const errorKey = pendingSuitRef.current ?? '_'
          setState(s => ({ ...s, errors: { ...s.errors, [errorKey]: msg } }))
          break
        }

        case 'waiting_for_start': {
          const w: WaitingForStartMessage = msg
          logger.info('ws/recv',
            `waiting_for_start connected=${w.connected} required=${w.required}`)
          setState(s => ({ ...s, waitingForStart: w }))
          break
        }

        case 'round_starting':
          logger.info('ws/recv',
            `round_starting starts_at=${msg.starts_at} player_count=${msg.player_count}`)
          // Clear waiting + inter-round state — the pre-deal countdown has begun.
          // Clearing interRound here dismisses InterRoundScreen so RoundCountdown
          // in the game header becomes visible (otherwise the overlay covers it).
          setState(s => ({ ...s, startsAt: msg.starts_at ?? null, waitingForStart: null, interRound: null }))
          break

        case 'round_start':
          logger.info('ws/recv', `round_start player_slot=${msg.player_slot} round_end_at=${msg.round_end_at} balance=${msg.balance} game_mode=${msg.game_mode}`)
          setState(s => ({
            ...s,
            hand: msg.hand,
            initialHand: msg.hand,
            playerSlot: msg.player_slot,
            startsAt: null,
            roundEndAt: msg.round_end_at,
            roundEnd: null,
            balance: msg.balance,
            interRound: null,
            roster: msg.roster,
            deltas: [],
            allHandTotals: msg.all_hand_totals ?? [],
            allBalances: msg.all_balances ?? [],
            gameMode: msg.game_mode ?? null,
            trades: [],
            books: {},
            myOrders: [],
            bookDepths: {},
            mboLogs: {},
          }))
          break

        case 'round_end':
          logger.info('ws/recv', `round_end goal_suit=${msg.goal_suit} results=${msg.results.length}`)
          setState(s => {
            const own = msg.results.find(r => r.player_slot === s.playerSlot) ?? null
            return { ...s, roundEndAt: null, roundEnd: msg, balance: own?.balance ?? s.balance }
          })
          break

        case 'lobby_state':
          logger.info('ws/recv', `lobby_state lobby_id=${msg.lobby_id} status=${msg.status} players=${msg.players.length}`)
          setState(s => ({ ...s, lobbyState: msg }))
          break

        case 'player_joined': {
          logger.info('ws/recv', `player_joined lobby_id=${msg.lobby_id} player_id=${msg.player_id} username=${msg.username}`)
          const joined = msg
          setState(s => {
            if (!s.lobbyState || s.lobbyState.lobby_id !== joined.lobby_id) return s
            // Guard duplicate join — deduplicate by bot_uuid for bots, player_id for humans.
            const isDuplicate = joined.is_bot
              ? s.lobbyState.players.some(p => p.is_bot && p.bot_uuid === joined.bot_uuid)
              : s.lobbyState.players.some(p => !p.is_bot && p.player_id === joined.player_id)
            if (isDuplicate) return s
            return {
              ...s,
              lobbyState: {
                ...s.lobbyState,
                players: [
                  ...s.lobbyState.players,
                  {
                    player_id:      joined.player_id,
                    bot_uuid:       joined.bot_uuid ?? null,
                    username:       joined.username,
                    joined_at:      joined.joined_at,
                    is_bot:         joined.is_bot,
                    bot_difficulty: joined.bot_difficulty,
                  },
                ],
              },
            }
          })
          break
        }

        case 'player_left': {
          logger.info('ws/recv', `player_left lobby_id=${msg.lobby_id} player_id=${msg.player_id} username=${msg.username}`)
          const left = msg
          setState(s => {
            if (!s.lobbyState || s.lobbyState.lobby_id !== left.lobby_id) return s
            return {
              ...s,
              lobbyState: {
                ...s.lobbyState,
                players: s.lobbyState.players.filter(p =>
                  left.is_bot
                    ? !(p.is_bot && p.bot_uuid === left.bot_uuid)
                    : p.player_id !== left.player_id
                ),
              },
            }
          })
          break
        }

        case 'lobby_started':
          logger.info('ws/recv', `lobby_started lobby_id=${msg.lobby_id} code=${msg.code}`)
          setState(s => ({ ...s, lobbyStarted: msg }))
          break

        case 'inter_round':
          logger.info('ws/recv', `inter_round round=${msg.round_number} goal=${msg.goal_suit}`)
          setState(s => applyMessage(s, msg, tradeSeqRef))
          break

        case 'game_ended':
          logger.info('ws/recv', `game_ended rounds=${msg.rounds.length} standings=${msg.final_standings.length}`)
          setState(s => applyMessage(s, msg, tradeSeqRef))
          break

        case 'game_player_left':
          logger.info('ws/recv', `game_player_left slot=${msg.player_slot} username=${msg.username}`)
          setState(s => applyMessage(s, msg, tradeSeqRef))
          break

        case 'game_bot_joined':
          logger.info('ws/recv', `game_bot_joined slot=${msg.player_slot} username=${msg.username} difficulty=${msg.bot_difficulty}`)
          setState(s => applyMessage(s, msg, tradeSeqRef))
          break

        case 'session_error':
          logger.warn('ws/recv', `session_error message=${msg.message}`)
          setState(s => applyMessage(s, msg, tradeSeqRef))
          break

        case 'delta_update':
          setState(s => applyMessage(s, msg, tradeSeqRef))
          break

        case 'all_balances':
          setState(s => applyMessage(s, msg, tradeSeqRef))
          break

        case 'hand_totals':
          setState(s => applyMessage(s, msg, tradeSeqRef))
          break

        case 'spectator_count':
          setState(s => applyMessage(s, msg, tradeSeqRef))
          break

        case 'script_log':
          setState(s => applyMessage(s, msg, tradeSeqRef))
          break

        case 'reconnect_token': {
          logger.info('ws/recv', `reconnect_token expires_at=${msg.expires_at}`)
          setState(s => ({ ...s, reconnectTokenMsg: { token: msg.token, expires_at: msg.expires_at } }))
          break
        }

        case 'game_state_snapshot': {
          // State lives here (not in useReconnect) to keep a single source of truth for hand/books/etc.
          // Books derived from full bids/asks; myOrders reconstructed by filtering on player_slot.
          const snap: GameStateSnapshotMessage = msg
          logger.info('ws/recv', `game_state_snapshot slot=${snap.player_slot} timer=${snap.round_timer_remaining}`)

          const books: Record<string, BookState> = {}
          const myOrders: MyOrder[] = []

          for (const [suit, book] of Object.entries(snap.order_books)) {
            const sortedBids = [...book.bids].sort((a, b) => b.price - a.price)
            const sortedAsks = [...book.asks].sort((a, b) => a.price - b.price)
            books[suit] = {
              best_bid:      sortedBids[0]?.price      ?? null,
              best_ask:      sortedAsks[0]?.price      ?? null,
              best_bid_slot: sortedBids[0]?.player_slot ?? null,
              best_ask_slot: sortedAsks[0]?.player_slot ?? null,
            }
            for (const bid of book.bids) {
              if (bid.player_slot === snap.player_slot)
                myOrders.push({ order_id: bid.order_id, suit, side: 'buy', price: bid.price, qty: bid.qty, qty_remaining: bid.qty_remaining })
            }
            for (const ask of book.asks) {
              if (ask.player_slot === snap.player_slot)
                myOrders.push({ order_id: ask.order_id, suit, side: 'sell', price: ask.price, qty: ask.qty, qty_remaining: ask.qty_remaining })
            }
          }

          const roundEndAt = new Date(Date.now() + snap.round_timer_remaining * 1000).toISOString()

          setState(s => ({
            ...s,
            hand:          snap.hand,
            initialHand:   snap.hand,
            playerSlot:    snap.player_slot,
            startsAt:      null,
            roundEndAt,
            balance:       snap.all_balances[snap.player_slot] ?? s.balance,
            allBalances:   snap.all_balances,
            allHandTotals: snap.all_hand_totals ?? [],
            deltas:        snap.deltas,
            roster:        snap.roster,
            books,
            myOrders,
            gameMode:      snap.game_mode ?? s.gameMode,
            gameStateSnapshot: snap,
          }))
          break
        }

        case 'game_bot_replaced': {
          logger.info('ws/recv', `game_bot_replaced slot=${msg.slot_index} username=${msg.username}`)
          setState(s => ({
            ...s,
            roster: s.roster.map(r =>
              r.player_slot === msg.slot_index ? { ...r, username: msg.username } : r
            ),
            departedSlots: s.departedSlots.filter(slot => slot !== msg.slot_index),
          }))
          break
        }

        case 'reconnect_window_expired': {
          logger.info('ws/recv', 'reconnect_window_expired')
          setState(s => ({ ...s, reconnectWindowExpired: true }))
          break
        }

        case 'queue_joined': {
          const lobbyId = pendingQueueLobbyRef.current ?? ''
          logger.info('ws/recv', `queue_joined pos=${msg.position} lobby=${lobbyId}`)
          setState(s => ({
            ...s,
            queueState: { status: 'queued', lobbyId, position: msg.position, queueSize: msg.queue_size, playersAround: [] },
          }))
          break
        }
        case 'queue_position_update': {
          const lobbyId = pendingQueueLobbyRef.current ?? ''
          logger.info('ws/recv', `queue_position_update pos=${msg.position} lobby=${lobbyId}`)
          setState(s => ({
            ...s,
            queueState: { status: 'queued', lobbyId, position: msg.position, queueSize: msg.queue_size, playersAround: msg.players_around },
          }))
          break
        }
        case 'queue_left':
          logger.info('ws/recv', 'queue_left')
          pendingQueueLobbyRef.current = null
          setState(s => ({ ...s, queueState: { status: 'idle' } }))
          break
        case 'queue_overflow':
          logger.info('ws/recv', `queue_overflow — lobby ${msg.lobby_id} full`)
          pendingQueueLobbyRef.current = null
          setState(s => ({ ...s, queueState: { status: 'overflow', lobbyId: msg.lobby_id } }))
          break
        case 'queue_admitted': {
          logger.info('ws/recv', `queue_admitted slot=${msg.slot_index} lobby=${msg.lobby_id}`)
          pendingQueueLobbyRef.current = null
          setState(s => ({ ...s, queueState: { status: 'admitted', lobbyId: msg.lobby_id, slotIndex: msg.slot_index } }))
          break
        }

        case 'lobby_owner_changed': {
          logger.info('ws/recv', `lobby_owner_changed — new owner player_id=${msg.new_owner_player_id} (${msg.new_owner_username})`)
          setState(s => ({
            ...s,
            currentOwnerPlayerId: msg.new_owner_player_id,
            currentOwnerUsername: msg.new_owner_username,
          }))
          break
        }

        case 'lobby_settings_changed': {
          logger.info('ws/recv', `lobby_settings_changed lobby_id=${msg.lobby_id} spawn_bots_on_leave=${msg.spawn_bots_on_leave} game_mode=${msg.game_mode}`)
          setState(s => {
            if (!s.lobbyState || s.lobbyState.lobby_id !== msg.lobby_id) return s
            return {
              ...s,
              lobbyState: {
                ...s.lobbyState,
                spawn_bots_on_leave:  msg.spawn_bots_on_leave  ?? s.lobbyState.spawn_bots_on_leave,
                bot_spawn_difficulty: msg.bot_spawn_difficulty ?? s.lobbyState.bot_spawn_difficulty,
                game_mode:            msg.game_mode            ?? s.lobbyState.game_mode,
              },
            }
          })
          break
        }

        case 'eval_posterior_update':
          logger.info('ws/recv', `eval_posterior_update slot=${msg.player_slot} configs=${msg.configurations?.length}`)
          setState(s => ({ ...s, evalPosteriorUpdate: msg }))
          break

        case 'eval_accumulation_signal':
          logger.info('ws/recv', `eval_accumulation_signal players=${msg.players?.length}`)
          setState(s => ({ ...s, evalAccumulationSignal: msg }))
          break

        case 'eval_execution_guidance':
          logger.info('ws/recv', `eval_execution_guidance slot=${msg.player_slot} action=${msg.action} suit=${msg.suit}`)
          setState(s => ({ ...s, evalExecutionGuidance: msg.action ? msg : null }))
          break

        // Slice 14/15: market-data feed messages
        case 'book_depth':
        case 'book_depth_snapshot':
          setState(s => ({
            ...s,
            bookDepths: { ...s.bookDepths, [msg.suit]: { bids: msg.bids, asks: msg.asks } },
          }))
          break

        case 'order_added': {
          const entry: MboLogEntry = {
            kind: 'added', seq: msg.seq, order_id: msg.order_id,
            price: msg.price, side: msg.side,
            owner_slot: msg.owner_slot, qty: msg.qty,
          }
          setState(s => ({
            ...s,
            mboLogs: { ...s.mboLogs, [msg.suit]: [entry, ...(s.mboLogs[msg.suit] ?? [])].slice(0, 100) },
          }))
          break
        }

        case 'order_executed': {
          const entry: MboLogEntry = {
            kind: 'executed', seq: msg.seq, order_id: msg.order_id, price: msg.price,
            qty_filled: msg.qty_filled, aggressor_order_id: msg.aggressor_order_id,
            buyer_slot: msg.buyer_slot, seller_slot: msg.seller_slot,
          }
          setState(s => ({
            ...s,
            mboLogs: { ...s.mboLogs, [msg.suit]: [entry, ...(s.mboLogs[msg.suit] ?? [])].slice(0, 100) },
          }))
          break
        }

        case 'order_cancelled': {
          const entry: MboLogEntry = { kind: 'cancelled', seq: msg.seq, order_id: msg.order_id, price: null }
          setState(s => ({
            ...s,
            mboLogs: { ...s.mboLogs, [msg.suit]: [entry, ...(s.mboLogs[msg.suit] ?? [])].slice(0, 100) },
          }))
          break
        }

        case 'order_book_snapshot':
          break

        case 'book_state_snapshot': {
          const snap = msg
          setState(s => {
            const newBooks = { ...s.books }
            for (const st of snap.suits) {
              newBooks[st.suit] = { best_bid: st.best_bid, best_ask: st.best_ask, best_bid_slot: st.best_bid_slot, best_ask_slot: st.best_ask_slot }
            }
            return { ...s, books: newBooks }
          })
          break
        }

        case 'order_partially_filled': {
          const pfMsg = msg
          logger.info('ws/recv', `order_partially_filled order_id=${pfMsg.order_id} qty_remaining=${pfMsg.qty_remaining}`)
          setState(s => {
            const updated = s.myOrders.map(o =>
              o.order_id === pfMsg.order_id ? { ...o, qty_remaining: pfMsg.qty_remaining } : o
            ).filter(o => o.qty_remaining > 0)
            return { ...s, myOrders: updated }
          })
          break
        }

        default: {
          // Exhaustiveness check — TypeScript errors here if a new ServerMessage variant is unhandled.
          const _exhaustive: never = msg
          logger.warn('ws/recv', 'unhandled message type', _exhaustive)
          break
        }
      }
    }

    return () => {
      logger.info('ws', 'closing connection (effect cleanup)')
      ws.close()
      // Same StrictMode guard: only null out the ref if it still points to this
      // socket. By the time this cleanup runs in StrictMode's remount cycle,
      // the new effect may have already assigned a fresh socket to wsRef.
      if (wsRef.current === ws) {
        wsRef.current = null
      }
    }
  }, [url])

  const sendMessage = useCallback((cmd: ClientCommand): void => {
    // Record which suit this command targets so an incoming error can be
    // keyed to the correct SuitPanel. Commands with no suit use null → '_'.
    pendingSuitRef.current = 'suit' in cmd ? cmd.suit : null

    const ws = wsRef.current
    if (ws && ws.readyState === WebSocket.OPEN) {
      const raw = JSON.stringify(cmd)
      logger.info('ws/send', `type=${cmd.type}`, cmd)
      ws.send(raw)
    } else {
      const state = ws ? ws.readyState : -1
      logger.warn('ws/send',
        `DROPPED — socket not open (readyState=${state}) cmd=${JSON.stringify(cmd)}`)
    }
  }, [])

  const subscribeLobby = useCallback((lobby_id: string) => {
    sendMessage({ type: 'subscribe_lobby', lobby_id })
  }, [sendMessage])

  const unsubscribeLobby = useCallback((lobby_id: string) => {
    sendMessage({ type: 'unsubscribe_lobby', lobby_id })
  }, [sendMessage])

  const joinQueue = useCallback((lobby_id: string) => {
    pendingQueueLobbyRef.current = lobby_id
    sendMessage({ type: 'join_queue', lobby_id })
  }, [sendMessage])

  const leaveQueue = useCallback((lobby_id: string) => {
    pendingQueueLobbyRef.current = null
    sendMessage({ type: 'leave_queue', lobby_id })
    setState(s => ({ ...s, queueState: { status: 'idle' } }))
  }, [sendMessage])

  const resetQueue = useCallback(() => {
    pendingQueueLobbyRef.current = null
    setState(s => ({ ...s, queueState: { status: 'idle' } }))
  }, [])

  // Derived per-suit self-trade guards. When the server adds best_bid_player /
  // best_ask_player fields (Slice 8), replace this derivation in one place here.
  const ownsBestBidBySuit: Record<string, boolean> = {}
  const ownsBestAskBySuit: Record<string, boolean> = {}
  for (const [suit, book] of Object.entries(state.books)) {
    const orders = state.myOrders.filter(o => o.suit === suit)
    ownsBestBidBySuit[suit] = book.best_bid !== null &&
      orders.some(o => o.side === 'buy'  && o.price === book.best_bid)
    ownsBestAskBySuit[suit] = book.best_ask !== null &&
      orders.some(o => o.side === 'sell' && o.price === book.best_ask)
  }

  return { ...state, sendMessage, ownsBestBidBySuit, ownsBestAskBySuit, subscribeLobby, unsubscribeLobby, joinQueue, leaveQueue, resetQueue }
}
