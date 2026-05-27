import type { EvalPosteriorUpdateMessage } from '../types/messages'
import './PosteriorDisplay.css'

const SUIT_SYMBOLS: Record<string, string> = {
  clubs: '♣', diamonds: '♦', hearts: '♥', spades: '♠',
}

interface Props {
  posteriorUpdate: EvalPosteriorUpdateMessage | null
}

export function PosteriorDisplay({ posteriorUpdate }: Props) {
  if (!posteriorUpdate) {
    return (
      <div className="posterior-display posterior-display--empty" data-testid="posterior-display">
        <span className="posterior-display__placeholder">Awaiting Bayesian update…</span>
      </div>
    )
  }

  const { configurations, goal_suit_marginals } = posteriorUpdate

  const top3 = [...configurations]
    .sort((a, b) => b.probability - a.probability)
    .slice(0, 3)

  const suits = ['clubs', 'diamonds', 'hearts', 'spades']

  return (
    <div className="posterior-display" data-testid="posterior-display">
      <h4 className="posterior-display__heading">Deck Configurations</h4>

      <div className="posterior-display__configs" data-testid="posterior-configs">
        {top3.map(cfg => (
          <div
            key={cfg.deck_index}
            className="posterior-display__config"
            data-testid={`posterior-config-${cfg.deck_index}`}
          >
            <div className="posterior-display__config-header">
              <span className={`posterior-display__goal-suit posterior-display__suit--${cfg.goal_suit}`}>
                {SUIT_SYMBOLS[cfg.goal_suit] ?? cfg.goal_suit}
              </span>
              <span className="posterior-display__deck-label">deck {cfg.deck_index}</span>
              <span className="posterior-display__prob">
                {(cfg.probability * 100).toFixed(1)}%
              </span>
            </div>
            <div className="posterior-display__bar-track">
              <div
                className="posterior-display__bar-fill"
                style={{ width: `${(cfg.probability * 100).toFixed(1)}%` }}
                data-testid={`posterior-bar-${cfg.deck_index}`}
              />
            </div>
            <div className="posterior-display__counts">
              {suits.map((s, i) => (
                <span
                  key={s}
                  className={`posterior-display__count-cell posterior-display__suit--${s}`}
                  title={s}
                >
                  {SUIT_SYMBOLS[s]}{cfg.counts[i]}
                </span>
              ))}
            </div>
          </div>
        ))}
      </div>

      <h4 className="posterior-display__heading">Goal Suit Marginals</h4>
      <div className="posterior-display__marginals" data-testid="posterior-marginals">
        {suits.map(s => (
          <div key={s} className="posterior-display__marginal-cell">
            <span className={`posterior-display__marginal-suit posterior-display__suit--${s}`}>
              {SUIT_SYMBOLS[s]}
            </span>
            <span className="posterior-display__marginal-prob" data-testid={`marginal-${s}`}>
              {((goal_suit_marginals[s] ?? 0) * 100).toFixed(0)}%
            </span>
          </div>
        ))}
      </div>
    </div>
  )
}
