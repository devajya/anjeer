import { MAX_TRADE_HISTORY } from '../constants'
import type {
  ServerMessage,
  BookUpdateMessage,
  TradeMessage,
  AllBalancesMessage,
} from '../types/messages'
import type { WsState, TradeEntry } from './useWebSocket'
/**
 * Pure reducer for ServerMessage cases shared between useWebSocket and useSpectator.
 * Returns the updated state, or the original state object if the message type is
 * not handled here (caller is responsible for hook-specific cases).
 *
 * tradeSeq is a mutable ref used to generate stable React keys for TradeEntry rows.
 */
export function applyMessage(
  s: WsState,
  msg: ServerMessage,
  tradeSeq: { current: number },
): WsState {
  switch (msg.type) {
    case 'book_update': {
      const u: BookUpdateMessage = msg
      return {
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
      }
    }

    case 'trade': {
      const trade: TradeMessage = msg
      const entry: TradeEntry = {
        id:             tradeSeq.current++,
        suit:           trade.suit,
        price:          trade.price,
        aggressor_side: trade.aggressor_side,
        your_side:      trade.your_side,
        buyer_slot:     trade.buyer_slot,
        seller_slot:    trade.seller_slot,
        qty_filled:     trade.qty_filled,
        qty_ordered:    trade.qty_ordered,
        ts:             Date.now(),
      }
      // Hand update: only when this player was a party to the trade.
      // Spectators have hand=null so the guard makes this a no-op for them.
      let hand = s.hand
      if (trade.your_side !== null && hand !== null) {
        const key = trade.suit as keyof typeof hand
        if (key in hand) {
          const delta = trade.your_side === 'buy' ? trade.qty_filled : -trade.qty_filled
          hand = { ...hand, [key]: hand[key] + delta }
        }
      }
      return {
        ...s,
        hand,
        trades:   [entry, ...s.trades].slice(0, MAX_TRADE_HISTORY),
        myOrders: s.gameMode === 'advanced' ? s.myOrders : [],
      }
    }

    case 'inter_round':
      return { ...s, interRound: msg, roundEnd: null, roundEndAt: null }

    case 'game_ended':
      return { ...s, gameEnded: msg, interRound: null }

    case 'game_player_left':
      return {
        ...s,
        departedSlots: s.departedSlots.includes(msg.player_slot)
          ? s.departedSlots
          : [...s.departedSlots, msg.player_slot],
      }

    case 'game_bot_joined':
      return {
        ...s,
        departedSlots: s.departedSlots.filter(slot => slot !== msg.player_slot),
        roster: s.roster.map(r =>
          r.player_slot === msg.player_slot ? { ...r, username: msg.username } : r
        ),
      }

    case 'session_error':
      return { ...s, sessionError: msg, interRound: null }

    case 'delta_update':
      return { ...s, deltas: msg.deltas }

    case 'all_balances': {
      const ab: AllBalancesMessage = msg
      return {
        ...s,
        allBalances: ab.balances,
        balance: s.playerSlot !== null ? (ab.balances[s.playerSlot] ?? s.balance) : s.balance,
      }
    }

    case 'hand_totals':
      return { ...s, allHandTotals: msg.totals }

    case 'spectator_count':
      return { ...s, spectatorCount: msg.count }

    case 'script_log':
      return { ...s, scriptLogs: [...(s.scriptLogs ?? []), msg] }

    default:
      return s
  }
}

