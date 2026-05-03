import type { HandCounts } from '../types/messages'
import { slotColorSemi } from '../utils/playerColors'
import './MarketOverview.css'

// AGENT-CTX: suit_index order mirrors engine::kAllSuits (0=clubs 1=diamonds
// 2=hearts 3=spades). Must stay in sync with DeltaTable and server broadcast.
const SUITS = [
  { key: 'clubs'    as const, index: 0, symbol: '♣', label: 'Clubs'    },
  { key: 'diamonds' as const, index: 1, symbol: '♦', label: 'Diamonds' },
  { key: 'hearts'   as const, index: 2, symbol: '♥', label: 'Hearts'   },
  { key: 'spades'   as const, index: 3, symbol: '♠', label: 'Spades'   },
] as const

interface RosterEntry { player_slot: number; username: string }

export interface MarketOverviewProps {
  hand: HandCounts | null
  initialHand: HandCounts | null
  // AGENT-CTX: deltas[player_slot][suit_index] — full snapshot from server.
  // Empty array before first round or before first trade.
  deltas: number[][]
  roster: RosterEntry[]
  mySlot: number | null
  balance: number | null
  /** Current balance per slot from all_balances broadcasts. */
  allBalances: number[]
  /** Total cards per slot from hand_totals broadcasts. */
  allHandTotals: number[]
}

function Delta({ value }: { value: number }) {
  if (value === 0) return null
  const pos = value > 0
  return (
    <span className={`mo__delta mo__delta--${pos ? 'pos' : 'neg'}`}>
      {pos ? '+' : '−'}{Math.abs(value)}
    </span>
  )
}

export function MarketOverview({ hand, initialHand, deltas, roster, mySlot, balance, allBalances, allHandTotals }: MarketOverviewProps) {
  const sortedRoster = [...roster].sort((a, b) => {
    if (a.player_slot === mySlot) return -1
    if (b.player_slot === mySlot) return 1
    return a.player_slot - b.player_slot
  })

  const myTotalCards = hand
    ? hand.clubs + hand.diamonds + hand.hearts + hand.spades
    : null

  function myDelta(key: typeof SUITS[number]['key']): number {
    if (!hand || !initialHand) return 0
    return hand[key] - initialHand[key]
  }

  function serverDelta(slot: number, suitIndex: number): number {
    return deltas[slot]?.[suitIndex] ?? 0
  }

  return (
    <div className="mo">
      {sortedRoster.length === 0 ? (
        <div className="mo__empty">Waiting for round to start…</div>
      ) : (
        <div className="mo__cards">
          {sortedRoster.map(({ player_slot, username }) => {
            const isMe = player_slot === mySlot
            const bg = slotColorSemi(player_slot)

            // Prefer per-slot broadcasts; fall back to local state for own player
            const slotBalance   = allBalances[player_slot]   ?? (isMe ? balance    : null)
            const slotCardTotal = allHandTotals[player_slot] ?? (isMe ? myTotalCards : null)

            return (
              <div key={player_slot} className={`mo__card${isMe ? ' mo__card--me' : ''}`}>

                {/* ── Card header: name + card count + balance inline ── */}
                <div className="mo__card-header" style={{ background: bg }}>
                  <span className="mo__card-name" title={username}>{username}</span>
                  <div className="mo__card-stats">
                    {slotCardTotal !== null && (
                      <span className="mo__stat mo__stat--cards" title="Total cards in hand">
                        <span className="mo__stat-icon mo__stat-icon--cards">▣</span>
                        <span className="mo__stat-val">{slotCardTotal}</span>
                      </span>
                    )}
                    {slotBalance !== null && (
                      <span className="mo__stat mo__stat--cash" title="Available cash">
                        <span className="mo__stat-icon mo__stat-icon--cash">$</span>
                        <span className="mo__stat-val">{slotBalance}</span>
                      </span>
                    )}
                  </div>
                </div>

                {/* ── Card body: one row per suit ── */}
                <div className="mo__card-body">
                  {SUITS.map(({ key, index, symbol }) => {
                    const delta = isMe ? myDelta(key) : serverDelta(player_slot, index)
                    const cardCount = isMe && hand ? hand[key] : null

                    return (
                      <div key={key} className="mo__card-row">
                        <div className={`mo__suit-circle mo__suit-circle--${key}`}>
                          {symbol}
                        </div>
                        <div className="mo__card-values">
                          {cardCount !== null && (
                            <span className="mo__card-count">{cardCount}</span>
                          )}
                          <Delta value={delta} />
                        </div>
                      </div>
                    )
                  })}
                </div>

              </div>
            )
          })}
        </div>
      )}
    </div>
  )
}
