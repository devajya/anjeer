import { useState, useEffect, useRef } from 'react'
import type { ServerMessage } from '../types/messages'
import type { WsState } from './useWebSocket'
import { logger } from '../logger'
import { applyMessage } from './useWsReducer'

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
  gameEnded: null,
  sessionError: null,
  departedSlots: [],
  deltas: [],
  roster: [],
  allHandTotals: [],
  allBalances: [],
  spectatorCount: 0,
  scriptLogs: [],
  // Reconnect/owner fields: spectators never receive these; fixed null/false.
  reconnectTokenMsg:    null,
  gameStateSnapshot:    null,
  reconnectWindowExpired: false,
  queueState:           { status: 'idle' },
  currentOwnerPlayerId: null,
  currentOwnerUsername: '',
  evalPosteriorUpdate:    null,
  evalAccumulationSignal: null,
  evalExecutionGuidance:  null,
  bookDepths: {},
  feedTier:   null,
  mboLogs:    {},
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

        case 'book_update':
          setState(s => applyMessage(s, msg, tradeSeqRef))
          break

        case 'trade':
          setState(s => applyMessage(s, msg, tradeSeqRef))
          break

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
          setState(s => applyMessage(s, msg, tradeSeqRef))
          break

        case 'game_ended':
          setState(s => applyMessage(s, msg, tradeSeqRef))
          break

        case 'game_player_left':
          setState(s => applyMessage(s, msg, tradeSeqRef))
          break

        case 'game_bot_joined':
          setState(s => applyMessage(s, msg, tradeSeqRef))
          break

        case 'session_error':
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

        // Slice 10.5 — not delivered to spectators; stubs for exhaustiveness.
        case 'reconnect_token':
        case 'game_state_snapshot':
        case 'game_bot_replaced':
        case 'queue_joined':
        case 'queue_left':
        case 'queue_position_update':
        case 'queue_overflow':
        case 'queue_admitted':
        case 'reconnect_window_expired':
        case 'lobby_owner_changed':
        case 'lobby_settings_changed':
        case 'eval_posterior_update':
        case 'eval_accumulation_signal':
        case 'eval_execution_guidance':
        // Slice 14: market-data feed tier messages — no spectator UI yet
        case 'book_depth':
        case 'book_depth_snapshot':
        case 'order_added':
        case 'order_executed':
        case 'order_cancelled':
        case 'order_book_snapshot':
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
