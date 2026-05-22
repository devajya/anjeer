import { useNavigate } from 'react-router-dom'
import './LearnPage.css'

export function LearnPage() {
  const navigate = useNavigate()

  return (
    <div className="learn">
      <header className="learn__header">
        <button className="learn__back" onClick={() => navigate('/lobby')}>← Lobby</button>
        <h1 className="learn__title">Understanding Your Eval Statistics</h1>
      </header>

      <nav className="learn__toc">
        <ol>
          <li><a href="#how-it-works">How the Model Works</a></li>
          <li><a href="#reading-the-numbers">Reading the Numbers</a></li>
          <li><a href="#limitations">Limitations</a></li>
          <li><a href="#roadmap">Planned Improvements</a></li>
        </ol>
      </nav>

      <main className="learn__body">

        <p className="learn__intro">
          The eval panel (toggle it open on the right side of the game screen) runs a live
          statistical model that tracks the most likely deck configuration, estimates your
          expected payout, and shows how much each additional card is worth. This page
          explains how it works, what the numbers mean, and where to be careful.
        </p>

        {/* ── How the model works ─────────────────────────────────────── */}
        <section id="how-it-works">
          <h2>How the Model Works</h2>
          <p>
            Before the round starts you know your hand but not the goal suit.
            The model uses{' '}
            <a href="https://en.wikipedia.org/wiki/Bayesian_inference" target="_blank" rel="noreferrer">
              Bayesian inference
            </a>
            {' '}to turn your hand into a probability distribution over all 12 possible
            deck configurations. The key steps are:
          </p>
          <ol>
            <li>
              <strong>Flat starting belief (<a href="https://en.wikipedia.org/wiki/Prior_probability" target="_blank" rel="noreferrer">uniform prior</a>).</strong>{' '}
              Before observing anything the model assigns equal probability — 1/12 — to every
              deck configuration. No configuration is favoured over another.
            </li>
            <li>
              <strong>Likelihood from your hand (<a href="https://en.wikipedia.org/wiki/Hypergeometric_distribution#Multivariate_hypergeometric_distribution" target="_blank" rel="noreferrer">multivariate hypergeometric sampling</a>).</strong>{' '}
              Each deck configuration specifies exactly how many cards of each suit exist in
              the full deck. The model asks: if this were the true configuration, how likely
              is it that you would have been dealt your specific hand? This likelihood is
              computed via the{' '}
              <a href="https://en.wikipedia.org/wiki/Hypergeometric_distribution" target="_blank" rel="noreferrer">
                hypergeometric distribution
              </a>
              , which models sampling cards without replacement from a finite population.
              Configurations that make your hand improbable get downweighted.
            </li>
            <li>
              <strong>Updated belief (<a href="https://en.wikipedia.org/wiki/Posterior_probability" target="_blank" rel="noreferrer">posterior probability</a>).</strong>{' '}
              Bayes' rule combines the prior with the likelihoods to produce a posterior —
              a new probability for each configuration that accounts for both the baseline
              frequency and your hand evidence.{' '}
              <a href="https://en.wikipedia.org/wiki/Bayes%27_theorem" target="_blank" rel="noreferrer">
                Bayes' theorem
              </a>
              {' '}guarantees this is the optimal update given the model's assumptions.
            </li>
            <li>
              <strong>Trade signal (heuristic nudge).</strong>{' '}
              After each trade, the model slightly upweights configurations where the buyer's
              purchased suit is the goal suit. This nudge decays toward zero as the round
              progresses (early trades reveal intent more than late ones). This is a
              hand-tuned heuristic — see limitations below.
            </li>
          </ol>
        </section>

        {/* ── Reading the numbers ─────────────────────────────────────── */}
        <section id="reading-the-numbers">
          <h2>Reading the Numbers</h2>

          <h3>Deck Configurations</h3>
          <p>
            The three deck layouts your hand is most consistent with, ranked by posterior
            probability. Each card shows: the goal suit symbol, the deck index, the probability,
            a bar representing that probability, and the suit distribution
            (e.g. <em>♣12 ♦8 ♥10 ♠8</em>).
          </p>
          <p className="learn__howto">
            <strong>How to use it:</strong> If one configuration dominates at 70%+, treat
            its goal suit as highly reliable. If the top three are all near 25–33%, you have
            genuine uncertainty and should hedge across suits until more trades reveal intent.
          </p>

          <h3>Goal Suit Marginals</h3>
          <p>
            The four-cell row below the deck configs shows the{' '}
            <a href="https://en.wikipedia.org/wiki/Marginal_distribution" target="_blank" rel="noreferrer">
              marginal probability
            </a>{' '}
            that each suit is the goal suit, obtained by summing posterior weight across every
            configuration that shares that goal suit. The four cells always sum to 100%.
          </p>
          <p className="learn__howto">
            <strong>How to use it:</strong> This is usually the first thing to look at.
            The suit with the highest marginal is your primary accumulation target.
            A marginal above ~60% is a strong signal; below ~40% means the market is still
            genuinely ambiguous.
          </p>

          <h3>Settlement EV</h3>
          <p>
            Expected payout in points from the goal cards you hold:{' '}
            <code>Σ P(config) × goal_cards_held × points_per_card</code>.
            Think of it as "if the round ended right now, this is your statistical
            expectation of what you'd earn from goal-card scoring — before the bonus pool."
          </p>
          <p className="learn__howto">
            <strong>How to use it:</strong> Compare settlement EV to the round buy-in to
            gauge whether you are currently in a profitable position. A settlement EV below
            the buy-in means you are losing money in expectation and should trade aggressively
            to accumulate goal cards.
          </p>

          <h3>Marginal Card EV (Δ EV per suit)</h3>
          <p>
            For each suit, the{' '}
            <a href="https://en.wikipedia.org/wiki/Expected_value" target="_blank" rel="noreferrer">
              expected value
            </a>{' '}
            of acquiring one additional card of that suit:{' '}
            <code>Σ P(config where goal=suit) × points_per_card</code>.
            Green means positive (acquiring the card improves your expectation), red means
            negative (acquiring it doesn't help because those configs are unlikely).
          </p>
          <p className="learn__howto">
            <strong>How to use it:</strong> Delta EV is your rational bid ceiling. If spades
            shows +18 and someone is asking 15, accepting that trade has positive expected
            value. If someone is asking 22 for spades, you would be overpaying relative to
            the model's estimate. Similarly, your sell floor for a suit is its delta EV —
            selling below it is giving away value.
          </p>
        </section>

        {/* ── Limitations ─────────────────────────────────────────────── */}
        <section id="limitations">
          <h2>Limitations</h2>

          <div className="learn__caveat">
            <h3>Settlement EV uses your starting hand, not your live hand</h3>
            <p>
              <em>Technical:</em> The module records your hand at the moment the round starts
              and does not update it as you trade. Settlement EV and delta EV are computed
              against that snapshot, so they undercount goal cards you have acquired mid-round.
            </p>
            <p className="learn__caveat-player">
              <strong>In plain terms:</strong> If you've been buying your target suit all
              round, the settlement EV shown is lower than your real position. Don't use it
              as a reason to stop buying — your actual expected payout is higher than what
              the panel shows.
            </p>
          </div>

          <div className="learn__caveat">
            <h3>Each player's posterior is computed independently</h3>
            <p>
              <em>Technical:</em> All four hands were dealt from the same deck configuration.
              A fully joint model would condition on all four hands simultaneously, converging
              to the correct deck much faster. The current model conditions only on your own
              hand and treats the other players' hands as unobserved. This leaves information
              on the table.
            </p>
            <p className="learn__caveat-player">
              <strong>In plain terms:</strong> The model doesn't know what cards the other
              players are holding, so its confidence builds more slowly than it theoretically
              could. If you can observe that an opponent has many clubs, the model isn't
              factoring that in — you should mentally adjust upward on any suit they appear
              to be avoiding.
            </p>
          </div>

          <div className="learn__caveat">
            <h3>Trade direction is a heuristic, not a proper Bayesian update</h3>
            <p>
              <em>Technical:</em> When a player buys suit S, the model nudges posterior weight
              toward configurations where S is the goal suit. This nudge is hand-tuned (a
              fixed log-space boost of 0.3 scaled by a time-decay factor) rather than derived
              from a formal likelihood model of rational trading behaviour.
            </p>
            <p className="learn__caveat-player">
              <strong>In plain terms:</strong> If someone buys spades to bluff you — or simply
              because they got a good price — the model will incorrectly start to believe
              spades is more likely the goal suit. A player deliberately buying the wrong suit
              can manipulate the eval panel. Treat the trade signal as a soft hint, not ground
              truth.
            </p>
          </div>

          <div className="learn__caveat">
            <h3>Time-decay weight is frozen at round start</h3>
            <p>
              <em>Technical:</em> The decay function <code>t / (t + 60)</code> is meant to
              give less weight to trades made late in the round (when players may hedge or
              bluff). However, the time value is cached once when the round starts and is not
              refreshed per trade. All trades during the round receive the same decay weight,
              so late-round trades are slightly over-weighted relative to the design intent.
            </p>
            <p className="learn__caveat-player">
              <strong>In plain terms:</strong> Trades near the end of the round should be
              treated as weaker signals (players are desperate and may trade anything), but
              the model weights them equally to opening trades. Take late-round accumulation
              signals with extra scepticism.
            </p>
          </div>

          <div className="learn__caveat">
            <h3>No inter-round learning</h3>
            <p>
              <em>Technical:</em> The prior resets to a flat uniform distribution at the start
              of every round. A{' '}
              <a href="https://en.wikipedia.org/wiki/Dirichlet_distribution" target="_blank" rel="noreferrer">
                Dirichlet conjugate prior
              </a>{' '}
              would allow the model to update its baseline belief about which deck
              configurations appear more frequently across rounds — particularly useful in a
              long game where some configurations have already been played.
            </p>
            <p className="learn__caveat-player">
              <strong>In plain terms:</strong> Even if diamonds has been the goal suit four
              rounds in a row, the model starts round five thinking all suits are equally
              likely. The eval panel has no memory of past rounds. You do — use it.
            </p>
          </div>

          <div className="learn__caveat">
            <h3>No price signal</h3>
            <p>
              <em>Technical:</em> Trade price carries information (fierce bidding on a suit
              suggests multiple players consider it goal-likely), but the current model uses
              only trade direction (which suit changed hands) and ignores the price at which
              it cleared.
            </p>
            <p className="learn__caveat-player">
              <strong>In plain terms:</strong> If spades is trading at 40 when the average
              price is 20, that competitive premium isn't reflected in the eval numbers.
              Unusually high prices are a market signal the panel doesn't see — factor them
              in yourself.
            </p>
          </div>
        </section>

        {/* ── Planned improvements ────────────────────────────────────── */}
        <section id="roadmap">
          <h2>Planned Improvements</h2>
          <p>
            The following are known limitations with concrete plans, held back by specific
            technical blockers rather than design disagreements.
          </p>
          <table className="learn__table">
            <thead>
              <tr>
                <th>Improvement</th>
                <th>What it adds</th>
                <th>Current blocker</th>
              </tr>
            </thead>
            <tbody>
              <tr>
                <td><strong>Live hand refresh</strong></td>
                <td>
                  Recompute settlement EV and delta EV against your current hand after every
                  trade, not just your starting hand
                </td>
                <td>
                  Requires passing a full updated snapshot (with live per-slot hands) on
                  every trade event, not only at round boundaries. The eval thread currently
                  receives a lightweight trade record, not a full snapshot.
                </td>
              </tr>
              <tr>
                <td><strong>Joint posterior over all hands</strong></td>
                <td>
                  Use all four visible hands simultaneously to compute one shared posterior —
                  collapsing uncertainty roughly four times faster with the same evidence
                </td>
                <td>
                  Compute cost on the current dev machine (consumer laptop running WSL2)
                  puts this above the 500 µs per-event latency budget. Feasible on dedicated
                  server hardware; deferred until deployment target is confirmed.
                </td>
              </tr>
              <tr>
                <td><strong>Proper trade likelihood model</strong></td>
                <td>
                  Replace the heuristic nudge with a formal probability model of player
                  trading behaviour, making the trade signal manipulation-resistant and
                  calibrated
                </td>
                <td>
                  Requires a behavioural prior over player intent that interacts with bot
                  difficulty tiers. Deferred until the planned Manipulator bot tier is
                  implemented.
                </td>
              </tr>
              <tr>
                <td><strong>Time-decay refresh per trade</strong></td>
                <td>
                  Re-seed the decay weight at the moment each trade fires so late-round
                  trades are correctly down-weighted
                </td>
                <td>
                  Minor wire change: trade events need to carry a wall-clock timestamp.
                  Straightforward fix, low priority.
                </td>
              </tr>
              <tr>
                <td><strong>Price-based inference</strong></td>
                <td>
                  Incorporate trade prices as signals of goal-suit competition intensity
                </td>
                <td>
                  Requires a prior over willingness-to-pay, which depends jointly on
                  opponent balance and deck configuration — a substantially more complex
                  model than the current hand-only inference.
                </td>
              </tr>
              <tr>
                <td><strong>Inter-round Dirichlet prior</strong></td>
                <td>
                  Carry a running belief about deck configuration frequencies across rounds
                  so the prior is informative rather than flat from round two onward
                </td>
                <td>
                  Needs a persistent prior stored in the session (not the DB — eval is
                  fully in-memory). Deferred because the 12-deck table is currently fixed;
                  becomes higher priority if the deck set becomes dynamic.
                </td>
              </tr>
            </tbody>
          </table>
        </section>

      </main>
    </div>
  )
}
