import { useState, useEffect, useRef, useCallback } from 'react'
import type {
  ServerMessage,
  BookUpdateMessage,
  TradeMessage,
  ErrorMessage,
  RoundEndMessage,
  ClientCommand,
  HandCounts,
  WaitingForStartMessage,
  LobbyStateMessage,
  LobbyStartedMessage,
  InterRoundMessage,
  VoteTallyMessage,
  GameEndedMessage,
  SessionErrorMessage,
  AllBalancesMessage,
} from '../types/messages'
import { logger } from '../logger'

// ─── Domain types exposed by the hook ────────────────────────────────────────

export interface BookState {
  best_bid: number | null
  best_ask: number | null
  best_bid_slot: number | null
  best_ask_slot: number | null
}

// AGENT-CTX: MyOrder is a client-side snapshot of an acknowledged resting order.
// It is populated from order_ack messages and pruned on order_cancel_ack or trade.
// On a global wipe (any trade executes) the server clears ALL books, so myOrders
// is also cleared entirely. If per-suit-only wipe is adopted in a future slice,
// change the trade handler to filter by suit instead of clearing the whole array.
export interface MyOrder {
  order_id: number
  suit: string
  side: 'buy' | 'sell'
  price: number
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
  ts: number  // Date.now() at receipt — used for display only
}

export interface WsState {
  connected: boolean
  /** Transient player identity assigned by the server on connect. null until received. */
  playerId: number | null
  /**
   * Current best bid/ask per active suit, keyed by suit name (e.g. "S1").
   * Populated as book_update messages arrive. Empty on first render.
   * AGENT-CTX: Slice 3 adds 3 more suits — this map scales automatically.
   */
  books: Record<string, BookState>
  /**
   * Most recent trades, newest first. Capped at MAX_TRADE_HISTORY entries.
   * AGENT-CTX: Cap chosen to keep the feed readable and avoid unbounded growth.
   * If a persistent trade log is needed, that belongs in the server (Slice 12).
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
   * AGENT-CTX: Slice 6 replaces this with a per-lobby lobby_joined/left feed.
   */
  waitingForStart: WaitingForStartMessage | null
  /**
   * Full lobby snapshot. Set on lobby_state; players array is updated
   * incrementally by player_joined / player_left without a full re-fetch.
   * AGENT-CTX: Null when not subscribed to any lobby (e.g. on the Game page).
   */
  lobbyState: LobbyStateMessage | null
  /**
   * Set when lobby_started arrives. LobbyRoom watches this in a useEffect
   * and navigates to /game?lobby_id=... when lobby_id matches current lobby.
   * AGENT-CTX: Never cleared by the hook — cleared implicitly on next WS reconnect.
   * The lobby_id guard in LobbyRoom prevents spurious re-navigation.
   */
  lobbyStarted: LobbyStartedMessage | null
  /**
   * Set on inter_round; cleared when a new round_start arrives.
   * AGENT-CTX: Task 10 renders InterRoundScreen while this is non-null.
   */
  interRound: InterRoundMessage | null
  /** Updated on every vote_tally message. Superceded by interRound.vote_count on round end. */
  voteTally: VoteTallyMessage | null
  /** Set on game_ended; never cleared — the session is over at that point. */
  gameEnded: GameEndedMessage | null
  /** Set on session_error; never cleared — the session is over at that point. */
  sessionError: SessionErrorMessage | null
  /**
   * Slot indices of players who disconnected mid-game (game_player_left).
   * AGENT-CTX: Accumulates across the session so the UI can keep departed
   * players greyed out even after multiple departures.
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
  scriptLogs: import('../types/messages').ScriptLogMessage[]
}

// ─── Hook ─────────────────────────────────────────────────────────────────────

const MAX_TRADE_HISTORY = 20

// AGENT-CTX: url must be a relative path starting with '/' (e.g. '/ws').
// The hook constructs an absolute ws:// or wss:// URL from window.location so
// the same code works in dev (Vite proxy) and production (nginx proxy).
// Do NOT pass a hardcoded absolute URL — that breaks the proxy abstraction.
// AGENT-CTX: ownsBestBidBySuit / ownsBestAskBySuit are derived per-render from
// state.books and state.myOrders. They are not stored in WsState (they would
// need to be recomputed on every books or myOrders change anyway). Exposed in
// the return type so App.tsx can pass them to SuitPanel without prop-drilling
// through an intermediate component.
export type UseWebSocketReturn = WsState & {
  sendMessage:        (cmd: ClientCommand) => void
  ownsBestBidBySuit:  Record<string, boolean>
  ownsBestAskBySuit:  Record<string, boolean>
  /** Sends subscribe_lobby over the open WS. No-op if socket not open. */
  subscribeLobby:     (lobby_id: string) => void
  /** Sends unsubscribe_lobby. Called by LobbyRoom cleanup effect on unmount. */
  unsubscribeLobby:   (lobby_id: string) => void
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
    voteTally: null,
    gameEnded: null,
    sessionError: null,
    departedSlots: [],
    deltas: [],
    roster: [],
    allHandTotals: [],
    allBalances: [],
    spectatorCount: 0,
    scriptLogs: [],
  })

  // AGENT-CTX: wsRef holds the live WebSocket instance so sendMessage (defined
  // outside the effect) can call ws.send() without being recreated on every render.
  const wsRef = useRef<WebSocket | null>(null)
  // Local counter for TradeEntry.id — never reset, guarantees unique React keys.
  const tradeSeqRef = useRef(0)
  // Tracks which suit the most-recently sent command targeted, so that an
  // incoming error message can be keyed to the correct SuitPanel. Null for
  // commands with no suit (e.g. cancel_order), which store errors under '_'.
  const pendingSuitRef = useRef<string | null>(null)

  useEffect(() => {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
    const fullUrl = `${protocol}//${window.location.host}${url}`
    logger.info('ws', `connecting to ${fullUrl}`)

    const ws = new WebSocket(fullUrl)
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
        msg = JSON.parse(event.data as string) as ServerMessage
      } catch (err) {
        logger.error('ws', 'failed to parse server message', { raw: event.data, err })
        return
      }

      // AGENT-CTX: Exhaustive switch on msg.type. TypeScript enforces that every
      // variant of ServerMessage is handled. If a new type is added to the union
      // in messages.ts but not here, the `default` never-check will surface it
      // as a compile error. Do not remove the default branch.
      switch (msg.type) {
        case 'player_hello':
          logger.info('ws/recv', `player_hello player_id=${msg.player_id}`)
          setState(s => ({ ...s, playerId: msg.player_id }))
          break

        case 'order_ack':
          logger.info('ws/recv', `order_ack order_id=${msg.order_id} suit=${msg.suit} side=${msg.side} price=${msg.price}`)
          setState(s => ({
            ...s,
            errors: { ...s.errors, [msg.suit]: null },
            myOrders: [
              ...s.myOrders,
              { order_id: msg.order_id, suit: msg.suit, side: msg.side, price: msg.price },
            ],
          }))
          break

        case 'book_update': {
          // AGENT-CTX: Immutable update — spread the outer state and the inner
          // books map so React sees a new reference and re-renders only affected
          // components. A mutation of s.books would be invisible to React.
          const update: BookUpdateMessage = msg
          logger.info('ws/recv', `book_update suit=${update.suit} best_bid=${update.best_bid} best_ask=${update.best_ask}`)
          setState(s => ({
            ...s,
            books: {
              ...s.books,
              [update.suit]: {
                best_bid:      update.best_bid,
                best_ask:      update.best_ask,
                best_bid_slot: update.best_bid_slot,
                best_ask_slot: update.best_ask_slot,
              },
            },
          }))
          break
        }

        case 'trade': {
          const trade: TradeMessage = msg
          logger.info('ws/recv', `trade suit=${trade.suit} price=${trade.price} aggressor=${trade.aggressor_side} your_side=${trade.your_side}`)
          const entry: TradeEntry = {
            id: tradeSeqRef.current++,
            suit: trade.suit,
            price: trade.price,
            aggressor_side: trade.aggressor_side,
            your_side: trade.your_side,
            buyer_slot: trade.buyer_slot,
            seller_slot: trade.seller_slot,
            ts: Date.now(),
          }
          setState(s => {
            // Update hand counts when this player was a party to the trade.
            // Buyer gains one card; seller loses one. Observers unchanged.
            // AGENT-CTX: Inferred client-side from your_side to avoid a separate
            // hand_update wire message. Mirrors the server's transfer_card() call.
            let hand = s.hand
            if (trade.your_side !== null && hand !== null) {
              const key = trade.suit as keyof HandCounts
              if (key in hand) {
                const delta = trade.your_side === 'buy' ? 1 : -1
                hand = { ...hand, [key]: hand[key] + delta }
              }
            }
            return {
              ...s,
              hand,
              trades: [entry, ...s.trades].slice(0, MAX_TRADE_HISTORY),
              // AGENT-CTX: Global wipe — any trade clears all resting orders server-side.
              // Mirror that here so the MyOrders list stays consistent.
              myOrders: [],
            }
          })
          break
        }

        case 'order_cancel_ack': {
          logger.info('ws/recv', `order_cancel_ack order_id=${msg.order_id}`)
          const cancelMsg = msg
          setState(s => ({
            ...s,
            myOrders: s.myOrders.filter(o => o.order_id !== cancelMsg.order_id),
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
          // Clear waiting state — countdown has begun.
          setState(s => ({ ...s, startsAt: msg.starts_at ?? null, waitingForStart: null }))
          break

        case 'round_start':
          logger.info('ws/recv', `round_start player_slot=${msg.player_slot} round_end_at=${msg.round_end_at} balance=${msg.balance}`)
          setState(s => ({
            ...s,
            hand: msg.hand,
            initialHand: msg.hand,
            playerSlot: msg.player_slot,
            startsAt: null,
            roundEndAt: msg.round_end_at,
            roundEnd: null,
            balance: msg.balance,
            // AGENT-CTX: Clear inter-round screen when the next round begins.
            interRound: null,
            voteTally: null,
            roster: msg.roster,
            deltas: [],
            allHandTotals: msg.all_hand_totals ?? [],
            allBalances: msg.all_balances ?? [],
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
          logger.info('ws/recv', `inter_round round=${msg.round_number} goal=${msg.goal_suit} votes=${msg.vote_count}/${msg.votes_required}`)
          // AGENT-CTX: Clears roundEnd and roundEndAt so the game board does not
          // show the previous round_end modal while the inter-round screen is up.
          setState(s => ({ ...s, interRound: msg, roundEnd: null, roundEndAt: null }))
          break

        case 'vote_tally':
          logger.info('ws/recv', `vote_tally votes=${msg.votes}/${msg.required}`)
          setState(s => ({ ...s, voteTally: msg }))
          break

        case 'game_ended':
          logger.info('ws/recv', `game_ended rounds=${msg.rounds.length} standings=${msg.final_standings.length}`)
          setState(s => ({ ...s, gameEnded: msg, interRound: null }))
          break

        case 'game_player_left':
          logger.info('ws/recv', `game_player_left slot=${msg.player_slot} username=${msg.username}`)
          setState(s => ({
            ...s,
            departedSlots: s.departedSlots.includes(msg.player_slot)
              ? s.departedSlots
              : [...s.departedSlots, msg.player_slot],
          }))
          break

        case 'game_bot_joined':
          logger.info('ws/recv', `game_bot_joined slot=${msg.player_slot} username=${msg.username} difficulty=${msg.bot_difficulty}`)
          setState(s => ({
            ...s,
            departedSlots: s.departedSlots.filter(slot => slot !== msg.player_slot),
            roster: s.roster.map(r =>
              r.player_slot === msg.player_slot ? { ...r, username: msg.username } : r
            ),
          }))
          break

        case 'session_error':
          logger.warn('ws/recv', `session_error message=${msg.message}`)
          setState(s => ({ ...s, sessionError: msg, interRound: null }))
          break

        case 'delta_update':
          setState(s => ({ ...s, deltas: msg.deltas }))
          break

        case 'all_balances': {
          const ab: AllBalancesMessage = msg
          setState(s => ({
            ...s,
            allBalances: ab.balances,
            balance: s.playerSlot !== null ? (ab.balances[s.playerSlot] ?? s.balance) : s.balance,
          }))
          break
        }

        case 'hand_totals':
          setState(s => ({ ...s, allHandTotals: msg.totals }))
          break

        case 'spectator_count':
          setState(s => ({ ...s, spectatorCount: msg.count }))
          break

        case 'script_log':
          setState(s => ({
            ...s,
            scriptLogs: [...(s.scriptLogs ?? []), msg],
          }))
          break

        // ── Slice 10.5 stubs — full logic added in T11 ────────────────────
        // AGENT-CTX: These cases are intentionally no-ops here. useReconnect
        // (T11) and QueuePopup (T12) consume these messages directly via their
        // own state machines. useWebSocket is not the right place for reconnect
        // or queue state — it only handles session-level events.
        case 'reconnect_token':
        case 'game_state_snapshot':
        case 'game_bot_replaced':
        case 'queue_joined':
        case 'queue_left':
        case 'queue_position_update':
        case 'queue_overflow':
        case 'queue_admitted':
        case 'reconnect_window_expired':
          logger.info('ws/recv', `${msg.type} — handled by dedicated hook (T11/T12)`)
          break

        default: {
          // AGENT-CTX: Exhaustiveness check. TypeScript errors here if a new
          // ServerMessage variant is added but not handled above.
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

  // AGENT-CTX: sendMessage is stable across renders (useCallback + empty deps).
  // AGENT-CTX: readyState guard prevents silent drops — log a warning instead
  // so the log file shows when sends are attempted on a non-open socket.
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

  // AGENT-CTX: subscribeLobby/unsubscribeLobby are thin wrappers so callers do not
  // need to import SubscribeLobbyCommand / UnsubscribeLobbyCommand from messages.ts.
  // sendMessage is stable (empty-dep useCallback), so these are also stable.
  const subscribeLobby = useCallback((lobby_id: string) => {
    sendMessage({ type: 'subscribe_lobby', lobby_id })
  }, [sendMessage])

  const unsubscribeLobby = useCallback((lobby_id: string) => {
    sendMessage({ type: 'unsubscribe_lobby', lobby_id })
  }, [sendMessage])

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

  return { ...state, sendMessage, ownsBestBidBySuit, ownsBestAskBySuit, subscribeLobby, unsubscribeLobby }
}
