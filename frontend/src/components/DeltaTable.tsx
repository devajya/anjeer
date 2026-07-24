import { memo } from 'react'
import './DeltaTable.css'

// AGENT-CTX: suit_index order mirrors engine::kAllSuits (0=clubs 1=diamonds
// 2=hearts 3=spades). Never reorder without updating the server broadcast too.
const SUITS = [
  { index: 0, label: 'Clubs',    symbol: '♣' },
  { index: 1, label: 'Diamonds', symbol: '♦' },
  { index: 2, label: 'Hearts',   symbol: '♥' },
  { index: 3, label: 'Spades',   symbol: '♠' },
] as const

interface RosterEntry {
  player_slot: number
  username: string
}

export interface DeltaTableProps {
  // AGENT-CTX: deltas[player_slot][suit_index] — full snapshot from server,
  // reset to [] on each round_start. Empty array = no trades yet this round.
  deltas: number[][]
  roster: RosterEntry[]
  mySlot: number | null
}

function DeltaCell({ value }: { value: number }) {
  if (value === 0) return <span className="delta-table__zero">—</span>
  const positive = value > 0
  return (
    <span className={positive ? 'delta-table__pos' : 'delta-table__neg'}>
      {positive ? '▲' : '▼'}{Math.abs(value)}
    </span>
  )
}

export const DeltaTable = memo(function DeltaTable({ deltas, roster, mySlot }: DeltaTableProps) {
  if (roster.length === 0) return null

  // AGENT-CTX: Roster is pre-sorted by player_slot from the server. We keep
  // that order so column indices match delta_table indices without a lookup.
  const sortedRoster = [...roster].sort((a, b) => a.player_slot - b.player_slot)

  function getDelta(slot: number, suitIndex: number): number {
    return deltas[slot]?.[suitIndex] ?? 0
  }

  return (
    <div className="delta-table" aria-label="Player position deltas">
      <table className="delta-table__grid">
        <thead>
          <tr>
            {/* Empty corner cell above suit labels */}
            <th className="delta-table__corner" />
            {sortedRoster.map(({ player_slot, username }) => (
              <th
                key={player_slot}
                className={
                  player_slot === mySlot
                    ? 'delta-table__col-header delta-table__col-header--me'
                    : 'delta-table__col-header'
                }
                title={username}
              >
                <span className="delta-table__username">{username}</span>
                {player_slot === mySlot && (
                  <span className="delta-table__me-badge">you</span>
                )}
              </th>
            ))}
          </tr>
        </thead>
        <tbody>
          {SUITS.map(({ index, label, symbol }) => (
            <tr key={index} className="delta-table__row">
              <td className="delta-table__suit-label" title={label}>
                {symbol}
              </td>
              {sortedRoster.map(({ player_slot }) => (
                <td
                  key={player_slot}
                  className={
                    player_slot === mySlot
                      ? 'delta-table__cell delta-table__cell--me'
                      : 'delta-table__cell'
                  }
                >
                  <DeltaCell value={getDelta(player_slot, index)} />
                </td>
              ))}
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  )
})
