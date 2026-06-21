import { useState } from 'react'
import './RulesPage.css'

type Tab = 'overview' | 'rules' | 'walkthrough' | 'modes'

const TABS: { id: Tab; label: string }[] = [
  { id: 'overview',    label: 'Overview'    },
  { id: 'rules',       label: 'Rules'       },
  { id: 'walkthrough', label: 'Walkthrough' },
  { id: 'modes',       label: 'Game Modes'  },
]

function Attribution() {
  return (
    <div className="rules__attribution">
      <span className="rules__attribution-icon">ℹ</span>
      <p>
        Anjeer is based on{' '}
        <a href="https://figgie.com/how-to-play" target="_blank" rel="noopener noreferrer">
          Figgie
        </a>
        , a card trading game created by Jane Street. The rules presented here are a
        simplified version for quick reference — the original and authoritative rules
        live at{' '}
        <a href="https://figgie.com/how-to-play" target="_blank" rel="noopener noreferrer">
          figgie.com/how-to-play
        </a>
        . If anything here conflicts with the original, the original wins.
      </p>
    </div>
  )
}

function OverviewTab() {
  return (
    <div className="rules__body">
      <div className="rules__section">
        <h2>What is Anjeer?</h2>
        <p>
          Anjeer is a real-time multiplayer card trading game based on{' '}
          <a href="https://figgie.com/how-to-play" target="_blank" rel="noopener noreferrer" style={{ color: 'var(--color-accent)' }}>
            Figgie
          </a>
          , invented at Jane Street in 2013. Four players are dealt cards from a 40-card deck,
          then spend four minutes trading them in an open-outcry market — posting bids and offers,
          crossing spreads, and trying to corner the one suit that will pay out at round end.
        </p>
        <p>
          The catch: no one knows which suit is the goal until the round ends. You have to infer
          it from the deck&apos;s card distribution, your own hand, and how your opponents trade.
          Every chip you spend buying a card is a bet on your inference being right.
        </p>
      </div>

      <div className="rules__section">
        <h2>The Core Loop</h2>
        <p>
          Each round follows the same rhythm:
        </p>
        <table className="rules__table">
          <tbody>
            <tr>
              <td><strong>Deal</strong></td>
              <td>40 cards split evenly — 10 per player. The deck has one fat suit (12 cards), one lean suit (8 cards), and two normal suits (10 each).</td>
            </tr>
            <tr>
              <td><strong>Trade</strong></td>
              <td>4 minutes of live bidding and offering. Cards move between players one at a time (Simple mode) or in larger blocks (Intermediate/Advanced).</td>
            </tr>
            <tr>
              <td><strong>Reveal</strong></td>
              <td>The goal suit is revealed. The fat suit&apos;s color partner (e.g. Spades had 12 → goal is Clubs) is what everyone was fighting over.</td>
            </tr>
            <tr>
              <td><strong>Pay out</strong></td>
              <td>$10 per goal-suit card you hold, plus a majority bonus from the pot if you hold more than half of all goal-suit cards.</td>
            </tr>
          </tbody>
        </table>
      </div>

      <div className="rules__section">
        <h2>Why it&apos;s interesting</h2>
        <p>
          The goal suit is never directly announced — you deduce it from the card counts. The deck
          always has exactly one suit with 12 cards, and the goal suit is the same color as that
          suit (black ↔ black, red ↔ red). So if you can figure out which suit has 12 cards, you
          know the goal. But the 12-card suit and its partner are both more common in the deck —
          driving up prices for both simultaneously, obscuring which one is actually the goal.
        </p>
        <p>
          Observation, inference, and speed all matter. The player who figures out the goal suit
          earliest and trades into it cheapest wins consistently over time.
        </p>
      </div>
    </div>
  )
}

function RulesTab() {
  return (
    <div className="rules__body">
      <div className="rules__section">
        <h2>Setup</h2>
        <p>
          Four players each ante <strong>$50</strong> into a shared <strong>$200 pot</strong>.
          A 40-card deck is shuffled and dealt face-down — 10 cards per player. Only your own
          hand is visible to you during trading.
        </p>
        <div className="rules__suits">
          <span className="rules__suit rules__suit--black">♠ Spades</span>
          <span className="rules__suit rules__suit--black">♣ Clubs</span>
          <span className="rules__suit rules__suit--red">♥ Hearts</span>
          <span className="rules__suit rules__suit--red">♦ Diamonds</span>
        </div>
      </div>

      <div className="rules__section">
        <h2>The Deck</h2>
        <p>
          Every round, one suit is assigned 12 cards, one gets 8, and the remaining two get 10
          each. The specific assignment is random and unknown to players. Because the total is
          40 cards across 4 players, most rounds deal an even 10 per player — but the <em>suit
          distribution</em> within each hand will vary.
        </p>
        <table className="rules__table">
          <thead>
            <tr>
              <th>Suit count</th>
              <th>Frequency</th>
              <th>Meaning</th>
            </tr>
          </thead>
          <tbody>
            <tr>
              <td>12 cards</td>
              <td>1 suit</td>
              <td>The &quot;fat&quot; suit — its color partner is the goal</td>
            </tr>
            <tr>
              <td>10 cards</td>
              <td>2 suits</td>
              <td>Normal suits</td>
            </tr>
            <tr>
              <td>8 cards</td>
              <td>1 suit</td>
              <td>The &quot;lean&quot; suit — same color as the goal</td>
            </tr>
          </tbody>
        </table>
      </div>

      <div className="rules__section">
        <h2>The Goal Suit</h2>
        <p>
          The goal suit is the <em>color partner</em> of the 12-card suit. Color pairing:
        </p>
        <table className="rules__table">
          <thead>
            <tr><th>If this suit has 12 cards…</th><th>…then the goal suit is</th></tr>
          </thead>
          <tbody>
            <tr><td>♠ Spades (black)</td><td>♣ Clubs (black)</td></tr>
            <tr><td>♣ Clubs (black)</td><td>♠ Spades (black)</td></tr>
            <tr><td>♥ Hearts (red)</td><td>♦ Diamonds (red)</td></tr>
            <tr><td>♦ Diamonds (red)</td><td>♥ Hearts (red)</td></tr>
          </tbody>
        </table>
        <p>
          Neither the 12-card suit nor the goal suit is revealed during the round. You infer
          which color family is &quot;hot&quot; by watching prices and your opponents&apos; trading behavior.
        </p>
      </div>

      <div className="rules__section">
        <h2>Trading</h2>
        <p>
          The trading phase lasts <strong>4 minutes</strong>. During this time you can post
          a <em>bid</em> (offer to buy a card at a price) or an <em>ask</em> (offer to sell a
          card at a price) for any suit.
        </p>
        <p>
          When a bid and ask for the same suit cross — the bidder&apos;s price meets or exceeds the
          asker&apos;s price — a trade executes. One card of that suit moves from the seller to the
          buyer; chips move the other way.
        </p>
        <p>
          In <strong>Simple mode</strong> (the default), every trade wipes all four order books
          clean. Everyone re-posts from scratch after each trade. In more advanced modes,
          orders can persist and carry larger quantities — see Game Modes below.
        </p>
      </div>

      <div className="rules__section">
        <h2>Scoring</h2>
        <p>Only the goal suit pays out. The $200 pot is distributed at round end:</p>
        <div className="rules__callout">
          <div className="rules__callout-row">
            <span>Per goal-suit card held</span>
            <em>$10 each</em>
          </div>
          <div className="rules__callout-row">
            <span>Majority bonus (strict majority of all goal-suit cards in play)</span>
            <em>remaining pot</em>
          </div>
        </div>
        <p style={{ marginTop: '0.75rem' }}>
          <strong>Majority:</strong> if the goal suit has 10 cards in play, the threshold is 6
          (10 ÷ 2 + 1). The player who holds 6 or more wins the remaining pot after the
          per-card payouts are subtracted. If no one has a strict majority, the remaining pot
          is split evenly among the players who hold the most goal-suit cards.
        </p>
        <p>
          Cards of non-goal suits are worthless. Chips spent buying them are losses. Getting
          stuck holding 8 spades when clubs is the goal means $0 from this round.
        </p>
      </div>
    </div>
  )
}

function WalkthroughTab() {
  return (
    <div className="rules__body">
      <div className="rules__section">
        <h2>A Short Example Round</h2>
        <p>
          This walkthrough traces one round from deal to payout to show how the rules
          play out concretely.
        </p>
      </div>

      <div className="rules__section">
        <h2>The Deal</h2>
        <div className="rules__scenario">
          <span className="rules__scenario-label">Deck this round</span>
          <p>
            ♠ Spades: 12 cards &nbsp;·&nbsp; ♣ Clubs: 8 cards &nbsp;·&nbsp;
            ♥ Hearts: 10 cards &nbsp;·&nbsp; ♦ Diamonds: 10 cards
          </p>
          <p>
            <strong>Goal suit: ♣ Clubs</strong> — same color (black) as the 12-card suit (Spades).
            Nobody knows this yet.
          </p>
        </div>
        <p>Cards are shuffled and dealt. Each player gets 10:</p>
        <div className="rules__hand-grid">
          <div className="rules__hand">
            <div className="rules__hand-name">Alice</div>
            <div className="rules__hand-cards">♠4 ♣3 ♥2 ♦1</div>
          </div>
          <div className="rules__hand">
            <div className="rules__hand-name">Bob</div>
            <div className="rules__hand-cards">♠3 ♣2 ♥3 ♦2</div>
          </div>
          <div className="rules__hand">
            <div className="rules__hand-name">Carol</div>
            <div className="rules__hand-cards">♠3 ♣2 ♥3 ♦2</div>
          </div>
          <div className="rules__hand">
            <div className="rules__hand-name">Dave</div>
            <div className="rules__hand-cards">♠2 ♣1 ♥2 ♦5</div>
          </div>
        </div>
      </div>

      <div className="rules__section">
        <h2>Early Trading</h2>
        <p>
          Alice holds 4 spades — the most of any suit in her hand. She suspects spades might
          be the fat suit and starts buying clubs (its color partner, and the likely goal).
        </p>
        <p>
          Alice posts a bid: <strong>buy ♣ clubs at $9</strong>. Bob, holding 2 clubs and
          not sure they matter, posts an ask: <strong>sell ♣ clubs at $10</strong>.
          No trade yet — they haven&apos;t crossed.
        </p>
        <p>
          Alice raises to $10. The bid crosses the ask: <strong>trade executes</strong>.
          Alice pays Bob $10; Bob gives Alice 1 club card. Alice now holds 4 clubs. The
          order books clear.
        </p>
      </div>

      <div className="rules__section">
        <h2>Mid-Round</h2>
        <p>
          Carol notices Alice is aggressively buying clubs and spades. She starts buying clubs
          too, driving the price to $12–$13. Dave, holding 5 diamonds and unsure of the goal,
          tries to sell clubs he doesn&apos;t have — quickly realizes he has none and stops.
        </p>
        <p>
          With 2 minutes left, Alice holds 5 clubs, Carol holds 4. The club price is now $13.
          Bob and Dave are mostly out of clubs.
        </p>
      </div>

      <div className="rules__section">
        <h2>End of Round</h2>
        <div className="rules__scenario">
          <span className="rules__scenario-label">Final holdings (clubs only)</span>
          <div className="rules__hand-grid">
            <div className="rules__hand">
              <div className="rules__hand-name">Alice</div>
              <div className="rules__hand-cards">♣ 5 clubs</div>
            </div>
            <div className="rules__hand">
              <div className="rules__hand-name">Bob</div>
              <div className="rules__hand-cards">♣ 1 club</div>
            </div>
            <div className="rules__hand">
              <div className="rules__hand-name">Carol</div>
              <div className="rules__hand-cards">♣ 2 clubs</div>
            </div>
            <div className="rules__hand">
              <div className="rules__hand-name">Dave</div>
              <div className="rules__hand-cards">♣ 0 clubs</div>
            </div>
          </div>
          <p>Total clubs in play: 8. Majority threshold: 8 ÷ 2 + 1 = <strong>5</strong>.</p>
          <p>
            Alice holds exactly 5 — strict majority. She wins the pot remainder after
            per-card payouts.
          </p>
        </div>
      </div>

      <div className="rules__section">
        <h2>Payout</h2>
        <p>Per-card payouts first (8 total clubs × $10 = $80 distributed):</p>
        <table className="rules__table">
          <thead>
            <tr><th>Player</th><th>Clubs held</th><th>Card payout</th><th>Majority bonus</th><th>Total</th></tr>
          </thead>
          <tbody>
            <tr><td>Alice</td><td>5</td><td>$50</td><td>$120 (remaining pot)</td><td><strong>$170</strong></td></tr>
            <tr><td>Bob</td>  <td>1</td><td>$10</td><td>—</td><td>$10</td></tr>
            <tr><td>Carol</td><td>2</td><td>$20</td><td>—</td><td>$20</td></tr>
            <tr><td>Dave</td> <td>0</td><td>$0</td> <td>—</td><td>$0</td></tr>
          </tbody>
        </table>
        <p style={{ marginTop: '0.5rem' }}>
          Remaining pot: $200 − $80 (per-card) = $120 → goes entirely to Alice.
          Dave loses his $50 ante and any chips spent on diamonds. Alice nearly doubles
          hers.
        </p>
      </div>
    </div>
  )
}

function ModesTab() {
  return (
    <div className="rules__body">
      <div className="rules__section">
        <h2>Choosing a Mode</h2>
        <p>
          The lobby owner picks a game mode when creating a lobby. All players in that lobby
          use the same mode. The core scoring rules — $10 per card, majority bonus — are
          identical across all modes. What changes is how orders work.
        </p>
      </div>

      <div className="rules__section">
        <div className="rules__mode-cards">
          <div className="rules__mode-card">
            <div className="rules__mode-header">
              <span className="rules__mode-name">Simple</span>
              <span className="rules__mode-badge rules__mode-badge--beginner">Beginner</span>
            </div>
            <p className="rules__mode-desc">
              Every order is for exactly <strong>one card</strong>. The moment any trade
              executes, all four order books wipe — every resting bid and ask across every
              suit cancels instantly. Everyone re-posts from scratch after each trade.
            </p>
            <p className="rules__mode-detail">
              Best for: learning the game, quick sessions, and UI lobbies where the fast
              rhythm keeps things engaging without needing to manage a persistent book.
              The center panel is hidden in simple mode — you only see the best bid and
              ask per suit.
            </p>
          </div>

          <div className="rules__mode-card">
            <div className="rules__mode-header">
              <span className="rules__mode-name">Intermediate</span>
              <span className="rules__mode-badge rules__mode-badge--intermediate">Intermediate</span>
            </div>
            <p className="rules__mode-desc">
              Orders can be for <strong>multiple cards at once</strong>. The wipe-on-trade
              mechanic still applies — any executed trade clears all four books — but
              because orders can carry more cards, individual trades move more volume.
            </p>
            <p className="rules__mode-detail">
              The center panel shows the full <strong>depth ladder</strong> for each suit:
              every price level and the total cards resting at that level. Best for players
              comfortable with simple mode who want to see more of the book structure.
            </p>
          </div>

          <div className="rules__mode-card">
            <div className="rules__mode-header">
              <span className="rules__mode-name">Advanced</span>
              <span className="rules__mode-badge rules__mode-badge--advanced">Advanced</span>
            </div>
            <p className="rules__mode-desc">
              Multi-card orders supported and <strong>books are not wiped on trade</strong>.
              Resting orders persist after partial fills — only the filled quantity is
              removed. Books only clear at round boundaries.
            </p>
            <p className="rules__mode-detail">
              The center panel shows a live <strong>order-by-order event feed</strong>:
              every new order, every execution, every cancel as it happens. This is the
              highest-fidelity mode — suited for API bots and experienced traders who want
              to track queue depth and order flow in detail.
            </p>
          </div>
        </div>
      </div>

      <div className="rules__section">
        <h2>Mode Comparison</h2>
        <table className="rules__table">
          <thead>
            <tr>
              <th>Feature</th>
              <th>Simple</th>
              <th>Intermediate</th>
              <th>Advanced</th>
            </tr>
          </thead>
          <tbody>
            <tr>
              <td>Cards per order</td>
              <td>1</td>
              <td>1 or more</td>
              <td>1 or more</td>
            </tr>
            <tr>
              <td>Books after a trade</td>
              <td>Wiped</td>
              <td>Wiped</td>
              <td>Persist</td>
            </tr>
            <tr>
              <td>Center panel</td>
              <td>None</td>
              <td>Depth ladder</td>
              <td>Order feed</td>
            </tr>
            <tr>
              <td>Scoring rules</td>
              <td colSpan={3} style={{ textAlign: 'center', color: 'var(--color-muted)' }}>Identical across all modes</td>
            </tr>
          </tbody>
        </table>
      </div>
    </div>
  )
}

export function RulesPage() {
  const [tab, setTab] = useState<Tab>('overview')

  return (
    <div className="rules">
      <header className="rules__header">
        <h1 className="rules__title">How to Play</h1>
      </header>

      <Attribution />

      <div className="rules__tabs" role="tablist">
        {TABS.map(t => (
          <button
            key={t.id}
            role="tab"
            aria-selected={tab === t.id}
            className={`rules__tab${tab === t.id ? ' rules__tab--active' : ''}`}
            onClick={() => setTab(t.id)}
          >
            {t.label}
          </button>
        ))}
      </div>

      {tab === 'overview'    && <OverviewTab />}
      {tab === 'rules'       && <RulesTab />}
      {tab === 'walkthrough' && <WalkthroughTab />}
      {tab === 'modes'       && <ModesTab />}
    </div>
  )
}
