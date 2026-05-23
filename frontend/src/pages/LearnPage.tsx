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
          <li><a href="#intro">What these docs cover</a></li>
          <li><a href="#bayesian">Module 1 — Bayesian Posterior</a></li>
          <li><a href="#accumulation">Module 2 — Accumulation Signal</a></li>
          <li><a href="#execution">Module 3 — Execution Guidance</a></li>
          <li><a href="#roadmap">Planned Improvements</a></li>
        </ol>
      </nav>

      <main className="learn__body">

        {/* ── Intro ───────────────────────────────────────────────────── */}
        <section id="intro">
          <h2>What These Docs Cover</h2>
          <p>
            The eval panel (toggle it open on the right side of the game screen) runs three
            live statistical modules on a background thread. Each module analyses a different
            aspect of the market and produces its own set of numbers. This page documents all
            three, grouped by module, with the same structure for each: how the model works,
            what the numbers mean, and where to be careful.
          </p>
          <p>
            <strong>What these docs do not cover:</strong> raw order-book mechanics, scoring
            rules, or keyboard shortcuts — those are in the{' '}
            <a href="/docs">API &amp; scripting reference</a>. The eval panel is read-only
            analysis — it does not submit orders or influence the game.
          </p>
          <div className="learn__caveat">
            <h3>How to read the confidence levels</h3>
            <p>
              All three modules update continuously as trades happen. Numbers shown are
              estimates, not ground truth. The Bayesian module conditions only on your own
              hand; the accumulation module sees trade directions but not hands; the execution
              module sees the order book but not intent. Each module is independently calibrated
              — a high-confidence Bayesian posterior does not imply the execution module agrees
              on which suit to trade.
            </p>
            <p className="learn__caveat-player">
              <strong>Rule of thumb:</strong> Use all three panels together. If the Bayesian
              posterior points strongly at clubs, the accumulation signal shows a player
              hoarding clubs, and the execution module recommends buying clubs — that
              convergence is a strong signal. If they disagree, treat each independently and
              weight by its own limitations.
            </p>
          </div>
        </section>

        {/* ── Module 1 — Bayesian ─────────────────────────────────────── */}
        <section id="bayesian">
          <details open>
            <summary className="learn__module-header">
              <span className="learn__module-tag">Module 1</span>
              Bayesian Posterior
            </summary>

            <div className="learn__module-body">
              <p>
                Estimates the probability that each deck configuration is the true one,
                given your starting hand. Updates after every trade via a trade-direction
                heuristic. This is the foundation of goal-suit inference.
              </p>

              <h3>How the Model Works</h3>
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
                  is it that you would have been dealt your specific hand? Configurations that make
                  your hand improbable get downweighted.
                </li>
                <li>
                  <strong>Updated belief (<a href="https://en.wikipedia.org/wiki/Posterior_probability" target="_blank" rel="noreferrer">posterior probability</a>).</strong>{' '}
                  <a href="https://en.wikipedia.org/wiki/Bayes%27_theorem" target="_blank" rel="noreferrer">
                    Bayes' theorem
                  </a>{' '}
                  combines the prior with the likelihoods to produce a posterior — a new
                  probability for each configuration that accounts for both the baseline frequency
                  and your hand evidence.
                </li>
                <li>
                  <strong>Trade signal (heuristic nudge).</strong>{' '}
                  After each trade, the model slightly upweights configurations where the buyer's
                  purchased suit is the goal suit. This nudge decays toward zero as the round
                  progresses. This is a hand-tuned heuristic — see limitations below.
                </li>
              </ol>

              <h3>Reading the Numbers</h3>

              <h4>Deck Configurations</h4>
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

              <h4>Goal Suit Marginals</h4>
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

              <h4>Settlement EV</h4>
              <p>
                Expected payout in points from the goal cards you hold:{' '}
                <code>Σ P(config) × goal_cards_held × points_per_card</code>.
                Think of it as "if the round ended right now, this is your statistical
                expectation of what you'd earn from goal-card scoring — before the bonus pool."
              </p>
              <p className="learn__howto">
                <strong>How to use it:</strong> Compare settlement EV to the round buy-in to
                gauge whether you are currently in a profitable position. A settlement EV below
                the buy-in means you are losing money in expectation and should trade aggressively.
              </p>

              <h4>Marginal Card EV (Δ EV per suit)</h4>
              <p>
                For each suit, the{' '}
                <a href="https://en.wikipedia.org/wiki/Expected_value" target="_blank" rel="noreferrer">
                  expected value
                </a>{' '}
                of acquiring one additional card of that suit:{' '}
                <code>Σ P(config where goal=suit) × points_per_card</code>.
                Green means positive; red means negative.
              </p>
              <p className="learn__howto">
                <strong>How to use it:</strong> Delta EV is your rational bid ceiling. If spades
                shows +18 and someone is asking 15, accepting that trade has positive expected
                value. If someone is asking 22 for spades, you would be overpaying relative to
                the model's estimate. Your sell floor for a suit is its delta EV — selling below
                it is giving away value.
              </p>

              <h3>Limitations</h3>

              <div className="learn__caveat">
                <h3>Settlement EV uses your starting hand, not your live hand</h3>
                <p>
                  <em>Technical:</em> The module records your hand at round start and does not
                  update it as you trade. Settlement EV and delta EV are computed against that
                  snapshot, so they undercount goal cards you have acquired mid-round.
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
                  A fully joint model would condition on all four hands simultaneously. The current
                  model conditions only on your own hand and treats the other players' hands as
                  unobserved.
                </p>
                <p className="learn__caveat-player">
                  <strong>In plain terms:</strong> The model doesn't know what cards others are
                  holding, so its confidence builds more slowly than it theoretically could. If
                  you can observe that an opponent has many clubs, the model isn't factoring that
                  in — adjust mentally.
                </p>
              </div>

              <div className="learn__caveat">
                <h3>Trade direction is a heuristic, not a proper Bayesian update</h3>
                <p>
                  <em>Technical:</em> When a player buys suit S, the model nudges posterior weight
                  toward configurations where S is the goal suit. This nudge is hand-tuned (a
                  fixed log-space boost of 0.3 scaled by a time-decay factor) rather than derived
                  from a formal likelihood model.
                </p>
                <p className="learn__caveat-player">
                  <strong>In plain terms:</strong> A player deliberately buying the wrong suit
                  can manipulate the eval panel. Treat the trade signal as a soft hint, not
                  ground truth.
                </p>
              </div>

              <div className="learn__caveat">
                <h3>Time-decay weight is frozen at round start</h3>
                <p>
                  <em>Technical:</em> The decay function <code>t / (t + 60)</code> is cached once
                  when the round starts and is not refreshed per trade. All trades during the round
                  receive the same decay weight, so late-round trades are slightly over-weighted.
                </p>
                <p className="learn__caveat-player">
                  <strong>In plain terms:</strong> Trades near the end of the round should be
                  treated as weaker signals (players are desperate and may trade anything), but the
                  model weights them equally to opening trades. Take late-round signals with extra
                  scepticism.
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
                  configurations appear more frequently across rounds.
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
                  only trade direction and ignores the price at which it cleared.
                </p>
                <p className="learn__caveat-player">
                  <strong>In plain terms:</strong> If spades is trading at 40 when the average
                  price is 20, that competitive premium isn't reflected in the eval numbers. Factor
                  unusually high prices in yourself.
                </p>
              </div>
            </div>
          </details>
        </section>

        {/* ── Module 2 — Accumulation ─────────────────────────────────── */}
        <section id="accumulation">
          <details>
            <summary className="learn__module-header">
              <span className="learn__module-tag">Module 2</span>
              Accumulation Signal
            </summary>

            <div className="learn__module-body">
              <p>
                Tracks the net card flow for every player in every suit across the round and
                classifies each player's accumulation behaviour as Normal, Elevated, or High.
                This is a public signal — all players see the same output.
              </p>

              <h3>How the Model Works</h3>
              <p>
                After every trade, the module updates each active slot's signed delta for the
                traded suit (<code>+1</code> for buyer, <code>−1</code> for seller) and
                recomputes a concentration score:
              </p>
              <ol>
                <li>
                  <strong>Signed delta.</strong>{' '}
                  Cumulative net card flow this round, per player, per suit. Positive means net
                  buyer; negative means net seller.
                </li>
                <li>
                  <strong>Concentration score.</strong>{' '}
                  <code>max(|delta|) / (sum(|deltas|) + ε)</code> for each slot. A score near 1.0
                  means the player's activity is concentrated in a single suit; near 0.25 means
                  activity is spread evenly across all four suits.
                </li>
                <li>
                  <strong>EWMA baseline normalisation.</strong>{' '}
                  The module maintains an{' '}
                  <a href="https://en.wikipedia.org/wiki/Moving_average#Exponential_moving_average" target="_blank" rel="noreferrer">
                    exponential moving average
                  </a>{' '}
                  of absolute delta magnitude per suit across rounds. This baseline normalises the
                  concentration score so that a slow market round doesn't produce spurious High
                  signals from tiny activity.
                </li>
                <li>
                  <strong>Signal classification.</strong>{' '}
                  Score below 0.65 → Normal; 0.65–0.85 → Elevated; ≥ 0.85 → High. A confidence
                  weight <code>purity × (1 − e<sup>−k × strength</sup>)</code> scales the signal
                  so that 1 trade produces a much weaker signal than 8 trades in the same suit.
                </li>
              </ol>

              <h3>Reading the Numbers</h3>

              <h4>Player rows</h4>
              <p>
                Each row shows one active player's signal level and the suit they are most
                strongly accumulating. A grey row means Normal (no strong directional bias). An
                amber row means Elevated (possible intent forming). A red row means High
                (concentrated, repeated accumulation in a single suit).
              </p>
              <p className="learn__howto">
                <strong>How to use it:</strong> A High signal on a player buying clubs is weak
                evidence that clubs is their goal suit — they could be accumulating to sell later
                or to bluff. Combined with a high Bayesian marginal for clubs, it becomes much
                stronger. Watch for players whose signal rises and then flattens — they may have
                reached their accumulation target and stopped.
              </p>

              <h4>Suit column</h4>
              <p>
                The suit with the largest absolute signed delta for that player — the suit they
                have been most active in (buying or selling). A large positive delta in a suit
                is a buying signal; a large negative delta means they have been liquidating that
                suit.
              </p>
              <p className="learn__howto">
                <strong>How to use it:</strong> If two players both show High signals in clubs,
                they are likely competing for the same goal suit. This competition usually
                drives prices up and makes passive market-making in clubs more risky.
              </p>

              <h3>Limitations</h3>

              <div className="learn__caveat">
                <h3>Accumulation in the wrong suit is indistinguishable from bluffing</h3>
                <p>
                  <em>Technical:</em> The module tracks card flow, not intent. A player who
                  systematically buys spades to bluff will produce the same High/spades signal
                  as a player who genuinely needs spades.
                </p>
                <p className="learn__caveat-player">
                  <strong>In plain terms:</strong> Advanced players can deliberately inflate their
                  accumulation signal in a decoy suit. Cross-reference with the Bayesian module:
                  if the posterior doesn't support the suit a player is accumulating, they may be
                  bluffing.
                </p>
              </div>

              <div className="learn__caveat">
                <h3>Signal resets every round</h3>
                <p>
                  <em>Technical:</em> Signed deltas are zeroed at round start. The EWMA baseline
                  persists across rounds (it normalises activity), but the per-round signal itself
                  carries no information from previous rounds.
                </p>
                <p className="learn__caveat-player">
                  <strong>In plain terms:</strong> A player who accumulated heavily in diamonds
                  last round starts this round with a clean slate — their past pattern isn't
                  visible in the accumulation panel, only in your own memory.
                </p>
              </div>

              <div className="learn__caveat">
                <h3>Low-activity rounds produce noisy signals</h3>
                <p>
                  <em>Technical:</em> In a round with very few trades, even two purchases in the
                  same suit can push a player to Elevated. The EWMA baseline mitigates this by
                  scaling the confidence weight with trade volume, but early in a slow round the
                  signal is still coarse.
                </p>
                <p className="learn__caveat-player">
                  <strong>In plain terms:</strong> In the first minute of a slow round, treat
                  all accumulation signals as tentative. Wait for at least 5–6 trades before
                  acting on an Elevated or High classification.
                </p>
              </div>
            </div>
          </details>
        </section>

        {/* ── Module 3 — Execution ────────────────────────────────────── */}
        <section id="execution">
          <details>
            <summary className="learn__module-header">
              <span className="learn__module-tag">Module 3</span>
              Execution Guidance
            </summary>

            <div className="learn__module-body">
              <p>
                Analyses the current order book and recent trade flow to recommend whether to
                buy aggressively (cross the spread), make a passive limit order (post inside
                the spread), or hold. This is a public signal — all players see the same output.
              </p>

              <h3>How the Model Works</h3>
              <p>
                After every book update, the module recomputes four quantities for each suit and
                picks the highest-EV action:
              </p>
              <ol>
                <li>
                  <strong>Fill probability.</strong>{' '}
                  <code>fill_rate_ewma × exp(−spread / k)</code>, clamped to [0, 1]. The fill
                  rate is an{' '}
                  <a href="https://en.wikipedia.org/wiki/Moving_average#Exponential_moving_average" target="_blank" rel="noreferrer">
                    EWMA
                  </a>{' '}
                  of inverse inter-trade intervals (trades per second). Wide spreads reduce fill
                  probability exponentially with decay constant <code>k = 5</code>.
                </li>
                <li>
                  <strong>Aggressive buy cost.</strong>{' '}
                  <code>best_ask − proxy_fair_value</code> where proxy fair value is approximated
                  as <code>best_bid</code> (a pre-Bayesian proxy; the module has no access to
                  the posterior). This is the cost of crossing the spread immediately.
                </li>
                <li>
                  <strong>Passive EV.</strong>{' '}
                  <code>fill_probability × (spread / 2) − leakage_penalty</code>. The leakage
                  penalty increases by a fixed step each time a directional cross is observed in
                  that suit (meaning the market is moving against passive orders) and decays
                  toward zero when no directional crosses occur.
                </li>
                <li>
                  <strong>Recommendation.</strong>{' '}
                  "buy" if passive EV is positive and dominates aggressive cost; "sell" if the
                  symmetric condition holds for the ask side; "hold" if no two-sided market
                  exists or no clear positive EV action is available.
                </li>
              </ol>

              <h3>Reading the Numbers</h3>

              <h4>Action (buy / sell / hold)</h4>
              <p>
                The module's recommended action for the suit with the highest positive expected
                value this tick. "buy" means: passively posting a limit bid is currently
                expected to be profitable after accounting for fill probability and adverse
                selection. "hold" means: no suit has a positive passive EV given current book
                state.
              </p>
              <p className="learn__howto">
                <strong>How to use it:</strong> This is a short-term execution signal, not a
                strategic one. It tells you <em>how</em> to transact if you have already decided
                <em>what</em> to buy — use the Bayesian and accumulation panels to decide what,
                and this panel to decide how. A "buy" recommendation in a suit you don't want
                is irrelevant.
              </p>

              <h4>Suit and price</h4>
              <p>
                The suit the recommendation applies to and the price at which the action should
                be placed (best bid or best ask, depending on direction). Both are derived from
                the current order book snapshot.
              </p>
              <p className="learn__howto">
                <strong>How to use it:</strong> The price shown is the current best quote, not
                a target. If the book moves between when you read this and when you submit your
                order, the price will be stale. In fast markets, prefer aggressive orders over
                passive ones even when the module recommends passive — the fill probability
                calculation assumes a stable book.
              </p>

              <h3>Limitations</h3>

              <div className="learn__caveat">
                <h3>Fair value proxy ignores your Bayesian posterior</h3>
                <p>
                  <em>Technical:</em> The aggressive buy cost calculation uses <code>best_bid</code>
                  as a proxy for fair value. This is a rough approximation. The true fair value
                  for you depends on your posterior over deck configurations — if the Bayesian
                  module gives clubs 80% probability, a club card is worth much more than the
                  current mid-price.
                </p>
                <p className="learn__caveat-player">
                  <strong>In plain terms:</strong> The execution module doesn't know what your
                  goal suit is. It evaluates all four suits using the same mid-price proxy. If
                  your goal suit is highly likely, you should be willing to cross the spread at
                  a larger cost than the module suggests — the Bayesian EV calculation accounts
                  for this; the execution module does not.
                </p>
              </div>

              <div className="learn__caveat">
                <h3>Fill probability assumes the book is stable</h3>
                <p>
                  <em>Technical:</em> The EWMA fill rate is computed from historical trade
                  frequency. If the market is currently quiescent but is about to become active,
                  the fill probability will be understated. Conversely, after a burst of trades,
                  the EWMA will remain elevated for several subsequent ticks.
                </p>
                <p className="learn__caveat-player">
                  <strong>In plain terms:</strong> After a flurry of activity the module may
                  recommend passive posting even if the book has gone quiet. Check the order
                  book directly — if there are no resting orders, a passive recommendation is
                  not actionable.
                </p>
              </div>

              <div className="learn__caveat">
                <h3>Leakage penalty is suit-isolated and ignores cross-suit correlation</h3>
                <p>
                  <em>Technical:</em> The directional-cross leakage penalty is tracked
                  independently for each suit. If informed traders are systematically crossing in
                  clubs, the penalty for clubs rises, but the penalties for other suits are
                  unaffected — even though those informed traders may be selling the other suits
                  to fund their club purchases.
                </p>
                <p className="learn__caveat-player">
                  <strong>In plain terms:</strong> When you see an Elevated or High accumulation
                  signal in one suit, the execution module may still recommend passive posting in
                  the other suits without warning. Treat the leakage penalty as a per-suit signal
                  only, not a market-wide risk indicator.
                </p>
              </div>

              <div className="learn__caveat">
                <h3>No position awareness</h3>
                <p>
                  <em>Technical:</em> The execution module knows nothing about your current
                  holdings, your buy-in, or how many goal cards you already hold. Its
                  recommendations are the same for a player who holds 0 goal cards and one who
                  holds 8.
                </p>
                <p className="learn__caveat-player">
                  <strong>In plain terms:</strong> If you already hold the majority of goal cards,
                  switching to a sell strategy may be optimal — but the module will continue to
                  recommend buying as long as the passive EV is positive. Override it when your
                  position is already strong.
                </p>
              </div>
            </div>
          </details>
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
                <td><strong>Live hand refresh (M1)</strong></td>
                <td>
                  Recompute settlement EV and delta EV against your current hand after every
                  trade, not just your starting hand
                </td>
                <td>
                  Requires passing a full updated snapshot (with live per-slot hands) on
                  every trade event. The eval thread currently receives a lightweight trade
                  record, not a full snapshot.
                </td>
              </tr>
              <tr>
                <td><strong>Joint posterior (M1)</strong></td>
                <td>
                  Use all four visible hands simultaneously to compute one shared posterior —
                  collapsing uncertainty roughly four times faster with the same evidence
                </td>
                <td>
                  Compute cost on the current dev machine (consumer laptop running WSL2)
                  puts this above the 500 µs per-event latency budget. Deferred until
                  deployment target is confirmed.
                </td>
              </tr>
              <tr>
                <td><strong>Proper trade likelihood model (M1)</strong></td>
                <td>
                  Replace the heuristic nudge with a formal probability model of player
                  trading behaviour, making the trade signal manipulation-resistant and
                  calibrated
                </td>
                <td>
                  Requires a behavioural prior over player intent that interacts with bot
                  difficulty tiers. Deferred until the Manipulator bot tier is implemented.
                </td>
              </tr>
              <tr>
                <td><strong>Inter-round Dirichlet prior (M1)</strong></td>
                <td>
                  Carry a running belief about deck configuration frequencies across rounds
                  so the prior is informative rather than flat from round two onward
                </td>
                <td>
                  Needs a persistent prior stored in the session (not the DB). Becomes
                  higher priority if the deck set becomes dynamic.
                </td>
              </tr>
              <tr>
                <td><strong>Posterior-aware fair value (M3)</strong></td>
                <td>
                  Feed the Bayesian module's goal-suit marginals into the execution module so
                  aggressive buy cost reflects your actual EV, not a mid-price proxy
                </td>
                <td>
                  Requires a shared state channel between the Bayesian and Execution modules
                  on the eval worker thread — currently each module has no visibility into
                  the others' state.
                </td>
              </tr>
              <tr>
                <td><strong>Position-aware execution (M3)</strong></td>
                <td>
                  Factor current hand holdings into the execution recommendation so the
                  module can switch to sell guidance when a player already holds majority
                </td>
                <td>
                  Same as live hand refresh — requires a full snapshot on each trade event.
                </td>
              </tr>
            </tbody>
          </table>
        </section>

      </main>
    </div>
  )
}
