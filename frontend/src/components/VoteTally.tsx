// AGENT-CTX: VoteTally is a pure display widget — no state, no side effects.
// Intentionally separate from InterRoundScreen so Task 11's GameEndScreen can
// reuse it if a final-vote summary is needed in future.
interface VoteTallyProps {
  votes: number
  required: number
}

export function VoteTally({ votes, required }: VoteTallyProps) {
  return (
    <div className="vote-tally" aria-live="polite" aria-atomic="true">
      <span className="vote-tally__label">Votes to end game:</span>
      <span className="vote-tally__count">
        <strong>{votes}</strong> / {required}
      </span>
    </div>
  )
}
