import { useState, useEffect, useRef } from 'react'
import type { InterRoundMessage } from '../types/messages'
import { VoteTally } from './VoteTally'
import './InterRoundScreen.css'

// AGENT-CTX: Suit glyphs duplicated from RoundEndModal to keep each component
// self-contained. A shared constant module (e.g. src/lib/suits.ts) is the right
// fix, but only worth doing during a design-system pass, not here.
const SUIT_ICON: Record<string, string> = {
  clubs: '♣', diamonds: '♦', hearts: '♥', spades: '♠',
}

interface InterRoundScreenProps {
  interRound: InterRoundMessage
  // AGENT-CTX: liveVotes/liveVotesRequired are derived by Game.tsx from subsequent
  // vote_tally messages, which supersede the initial vote_count/votes_required
  // snapshot in interRound. Passed as separate props so this component is a
  // pure renderer — it never subscribes to the WS directly.
  liveVotes: number
  liveVotesRequired: number
  playerSlot: number | null
  // AGENT-CTX: hasVoted prevents the player from casting more than one
  // vote_to_end per inter-round window. Reset by Game.tsx on each new interRound.
  hasVoted: boolean
  onVoteToEnd: () => void
  // AGENT-CTX: Called once when the countdown reaches 0. Game.tsx sets
  // interRoundDismissed=true so the overlay closes locally without waiting for
  // round_start to arrive (which may lag a frame or two behind the timer).
  onCountdownExpired: () => void
}

export function InterRoundScreen({
  interRound,
  liveVotes,
  liveVotesRequired,
  playerSlot,
  hasVoted,
  onVoteToEnd,
  onCountdownExpired,
}: InterRoundScreenProps) {
  const [secondsLeft, setSecondsLeft] = useState<number | null>(null)

  // AGENT-CTX: Ref keeps onCountdownExpired out of the timer effect's dep array.
  // Without it, a new function identity on each render would teardown and restart
  // the interval every tick, breaking the countdown.
  const onExpiredRef = useRef(onCountdownExpired)
  useEffect(() => { onExpiredRef.current = onCountdownExpired }, [onCountdownExpired])

  useEffect(() => {
    if (!interRound.next_round_at) return

    const target = new Date(interRound.next_round_at).getTime()

    function tick(): boolean {
      const remaining = Math.ceil((target - Date.now()) / 1000)
      if (remaining <= 0) {
        setSecondsLeft(0)
        return true
      }
      setSecondsLeft(remaining)
      return false
    }

    if (tick()) {
      onExpiredRef.current()
      return
    }

    const id = setInterval(() => {
      if (tick()) {
        clearInterval(id)
        onExpiredRef.current()
      }
    }, 500)

    return () => clearInterval(id)
  // AGENT-CTX: next_round_at is stable across the inter-round window (comes from
  // the immutable InterRoundMessage). Including interRound identity here is safe —
  // Game.tsx only passes a new interRound when a new round ends.
  }, [interRound.next_round_at])

  const goalSuitDisplay =
    interRound.goal_suit.charAt(0).toUpperCase() + interRound.goal_suit.slice(1)

  // next_round_at is null when the vote majority was reached before/at round-end.
  const voteThresholdMet = interRound.next_round_at === null

  return (
    <div
      className="irs__backdrop"
      role="dialog"
      aria-modal="true"
      aria-label="Round results"
    >
      <div className="irs__card">
        <h2 className="irs__title">Round {interRound.round_number} Finished</h2>

        {/* ── Goal suit reveal ── */}
        <div className="irs__goal-suit">
          <span
            className={`irs__suit-icon irs__suit-icon--${interRound.goal_suit}`}
            aria-hidden="true"
          >
            {SUIT_ICON[interRound.goal_suit] ?? interRound.goal_suit}
          </span>
          <span className="irs__suit-name">{goalSuitDisplay} was the goal suit</span>
        </div>

        {/* ── Standings ── */}
        <table className="irs__standings">
          <thead>
            <tr>
              <th>Player</th>
              <th>Goal Cards</th>
              <th>Payout</th>
              <th>Balance</th>
            </tr>
          </thead>
          <tbody>
            {interRound.results.map(r => (
              <tr
                key={r.player_slot}
                className={[
                  'irs__row',
                  r.player_slot === playerSlot ? 'irs__row--own' : '',
                ].filter(Boolean).join(' ')}
              >
                <td>
                  Player {r.player_slot}
                  {r.disconnected && <span className="irs__away"> (away)</span>}
                </td>
                <td>{r.goal_cards_held}</td>
                <td>{r.payout}</td>
                <td>{r.balance}</td>
              </tr>
            ))}
          </tbody>
        </table>

        {/* ── Vote section ── */}
        <div className="irs__vote-section">
          <VoteTally votes={liveVotes} required={liveVotesRequired} />
          {!voteThresholdMet && (
            <button
              className="irs__vote-btn"
              onClick={onVoteToEnd}
              disabled={hasVoted}
            >
              {hasVoted ? 'Vote Cast' : 'Vote to End Game'}
            </button>
          )}
        </div>

        {/* ── Countdown / status line ── */}
        <div className="irs__status">
          {voteThresholdMet ? (
            <span className="irs__status--ending">
              Vote threshold reached — game ending…
            </span>
          ) : secondsLeft !== null && secondsLeft > 0 ? (
            <span>
              Next round in <strong>{secondsLeft}s</strong>
            </span>
          ) : secondsLeft === 0 ? (
            <span className="irs__status--starting">Starting next round…</span>
          ) : null}
        </div>
      </div>
    </div>
  )
}
