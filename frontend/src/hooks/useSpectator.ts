import { useState, useEffect, useRef } from 'react'
import type {
  ServerMessage,
  BookUpdateMessage,
  TradeMessage,
  AllBalancesMessage,
} from '../types/messages'
import type { WsState } from './useWebSocket'
import { logger } from '../logger'

const MAX_TRADE_HISTORY = 20

const INITIAL_STATE: WsState = {
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
}

// Spectator-only hook. Connects to /ws, sends spectate_lobby on open, and
// handles all public ServerMessage variants. Does not expose sendMessage —
// spectators cannot submit game commands.
export function useSpectator(lobbyId: string): WsState {
  const [state, setState] = useState<WsState>(INITIAL_STATE)
  const wsRef       = useRef<WebSocket | null>(null)
  const tradeSeqRef = useRef(0)

  useEffect(() => {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
    const fullUrl   = `${protocol}//${window.location.host}/ws`
    logger.info('spectator', `connecting to ${fullUrl} for lobby ${lobbyId}`)

    const ws = new WebSocket(fullUrl)
    wsRef.current = ws

    ws.onopen = () => {
      logger.info('spectator', 'connected — sending spectate_lobby')
      ws.send(JSON.stringify({ type: 'spectate_lobby', lobby_code: lobbyId }))
      setState(s => ({ ...s, connected: true }))
    }

    ws.onclose = (ev) => {
      logger.warn('spectator', `closed code=${ev.code} reason="${ev.reason}"`)
      setState(s => ({ ...s, connected: false }))
      if (wsRef.current === ws) wsRef.current = null
    }

    ws.onerror = (ev) => {
      logger.error('spectator', 'WebSocket error', ev)
    }

    ws.onmessage = (event: MessageEvent) => {
      let msg: ServerMessage
      try {
        msg = JSON.parse(event.data as string) as ServerMessage
      } catch {
        return
      }

      switch (msg.type) {
        case 'player_hello':
          logger.info('spectator/recv', `player_hello player_id=${msg.player_id} role=${msg.role}`)
          setState(s => ({ ...s, playerId: msg.player_id }))
          break

        case 'book_update': {
          const u: BookUpdateMessage = msg
          setState(s => ({
            ...s,
            books: {
              ...s.books,
              [u.suit]: {
                best_bid:      u.best_bid,
                best_ask:      u.best_ask,
                best_bid_slot: u.best_bid_slot,
                best_ask_slot: u.best_ask_slot,
              },
            },
          }))
          break
        }

        case 'trade': {
          const trade: TradeMessage = msg
          const entry = {
            id:             tradeSeqRef.current++,
            suit:           trade.suit,
            price:          trade.price,
            aggressor_side: trade.aggressor_side,
            your_side:      null as 'buy' | 'sell' | null,
            buyer_slot:     trade.buyer_slot,
            seller_slot:    trade.seller_slot,
            ts:             Date.now(),
          }
          setState(s => ({
            ...s,
            trades: [entry, ...s.trades].slice(0, MAX_TRADE_HISTORY),
          }))
          break
        }

        case 'round_starting': {
          const roster = msg.usernames
            ? msg.usernames.map((username, idx) => ({ player_slot: idx, username }))
            : undefined
          setState(s => ({
            ...s,
            startsAt:   msg.starts_at   ?? null,
            roundEndAt: msg.round_end_at ?? null,
            waitingForStart: null,
            ...(roster ? { roster } : {}),
          }))
          break
        }

        case 'round_start':
          setState(s => ({
            ...s,
            playerSlot:    msg.player_slot,
            startsAt:      null,
            roundEndAt:    msg.round_end_at,
            roundEnd:      null,
            interRound:    null,
            voteTally:     null,
            roster:        msg.roster,
            deltas:        [],
            allHandTotals: msg.all_hand_totals ?? [],
            allBalances:   msg.all_balances   ?? [],
          }))
          break

        case 'round_end':
          setState(s => ({ ...s, roundEndAt: null, roundEnd: msg }))
          break

        case 'inter_round':
          setState(s => ({ ...s, interRound: msg, roundEnd: null, roundEndAt: null }))
          break

        case 'vote_tally':
          setState(s => ({ ...s, voteTally: msg }))
          break

        case 'game_ended':
          setState(s => ({ ...s, gameEnded: msg, interRound: null }))
          break

        case 'game_player_left':
          setState(s => ({
            ...s,
            departedSlots: s.departedSlots.includes(msg.player_slot)
              ? s.departedSlots
              : [...s.departedSlots, msg.player_slot],
          }))
          break

        case 'game_bot_joined':
          setState(s => ({
            ...s,
            departedSlots: s.departedSlots.filter(slot => slot !== msg.player_slot),
            roster: s.roster.map(r =>
              r.player_slot === msg.player_slot ? { ...r, username: msg.username } : r
            ),
          }))
          break

        case 'session_error':
          setState(s => ({ ...s, sessionError: msg, interRound: null }))
          break

        case 'delta_update':
          setState(s => ({ ...s, deltas: msg.deltas }))
          break

        case 'all_balances': {
          const ab: AllBalancesMessage = msg
          setState(s => ({ ...s, allBalances: ab.balances }))
          break
        }

        case 'hand_totals':
          setState(s => ({ ...s, allHandTotals: msg.totals }))
          break

        case 'spectator_count':
          setState(s => ({ ...s, spectatorCount: msg.count }))
          break

        case 'script_log':
          setState(s => ({ ...s, scriptLogs: [...(s.scriptLogs ?? []), msg] }))
          break

        // Spectators never receive these; listed for type exhaustiveness.
        case 'order_ack':
        case 'order_cancel_ack':
        case 'error':
        case 'waiting_for_start':
        case 'lobby_state':
        case 'player_joined':
        case 'player_left':
        case 'lobby_started':
          break

        default: {
          const _exhaustive: never = msg
          logger.warn('spectator/recv', 'unhandled message type', _exhaustive)
        }
      }
    }

    return () => {
      logger.info('spectator', 'closing connection (effect cleanup)')
      ws.close()
      if (wsRef.current === ws) wsRef.current = null
    }
  }, [lobbyId])

  return state
}
