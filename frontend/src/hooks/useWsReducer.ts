import { MAX_TRADE_HISTORY, MBO_LOG_CAP } from '../constants'
import type {
  ServerMessage,
  BookUpdateMessage,
  TradeMessage,
  AllBalancesMessage,
} from '../types/messages'
import type { WsState, TradeEntry, MboLogEntry } from './useWebSocket'

/**
 * Applies one MBO event to both the capped event log and the incrementally
 * maintained live-order map. The map is the authoritative source for the price
 * ladder and top-of-book depth: replaying the log cannot reconstruct orders
 * that have aged past the MBO_LOG_CAP window.
 *
 * Returns the same liveOrders object identity when the event changes nothing,
 * so downstream memos are not invalidated by no-op events.
 */
export function applyMboEvent(
  s:     Pick<WsState, 'mboLogs' | 'liveOrders'>,
  suit:  string,
  entry: MboLogEntry,
): Pick<WsState, 'mboLogs' | 'liveOrders'> {
  return {
    mboLogs:    { ...s.mboLogs, [suit]: [entry, ...(s.mboLogs[suit] ?? [])].slice(0, MBO_LOG_CAP) },
    liveOrders: applyLiveOrder(s.liveOrders, suit, entry),
  }
}

function applyLiveOrder(
  liveOrders: WsState['liveOrders'],
  suit:       string,
  e:          MboLogEntry,
): WsState['liveOrders'] {
  const suitOrders = liveOrders[suit] ?? {}

  if (e.kind === 'added') {
    if (!e.side || e.price === null) return liveOrders
    return {
      ...liveOrders,
      [suit]: { ...suitOrders, [e.order_id]: { side: e.side, price: e.price, qty: e.qty ?? 1 } },
    }
  }

  if (e.kind === 'executed') {
    // Only the passive (resting) order is tracked here. An aggressor that rests
    // with a remainder arrives as its own 'added' event.
    const o = suitOrders[e.order_id]
    if (!o) return liveOrders
    const qty  = o.qty - (e.qty_filled ?? 1)
    const next = { ...suitOrders }
    if (qty <= 0) delete next[e.order_id]
    else          next[e.order_id] = { ...o, qty }
    return { ...liveOrders, [suit]: next }
  }

  if (!(e.order_id in suitOrders)) return liveOrders
  const next = { ...suitOrders }
  delete next[e.order_id]
  return { ...liveOrders, [suit]: next }
}
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
      let myOrders = s.myOrders
      if (s.gameMode === 'advanced' && trade.your_side !== null) {
        const passiveSide: 'buy' | 'sell' = trade.aggressor_side === 'buy' ? 'sell' : 'buy'
        const removedId = trade.your_side === passiveSide
          ? trade.passive_order_id
          : trade.aggressor_order_id
        const target = myOrders.find(o => o.suit === trade.suit && o.order_id === removedId)
        // Only remove if the order was fully consumed. For a partial fill of the aggressor's
        // multi-qty order the server sends order_partially_filled next; leave the order in
        // myOrders so that message can update qty_remaining and keep isLive=true.
        // For the passive side the server also sends order_partially_filled (even at qty_remaining=0),
        // so the same guard works for both sides.
        if (!target || target.qty_remaining <= trade.qty_filled) {
          myOrders = myOrders.filter(o => !(o.suit === trade.suit && o.order_id === removedId))
        }
      } else if (s.gameMode !== 'advanced') {
        myOrders = []
      }
      return {
        ...s,
        hand,
        trades:   [entry, ...s.trades].slice(0, MAX_TRADE_HISTORY),
        myOrders,
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
      return { ...s, scriptLogs: [...(s.scriptLogs ?? []), { ...msg, _seq: (s.scriptLogs ?? []).length }] }

    default:
      return s
  }
}

