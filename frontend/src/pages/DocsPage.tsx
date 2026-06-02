import './DocsPage.css'

export function DocsPage() {
  return (
    <div className="docs">
      <header className="docs__header">
        <h1 className="docs__title">Anjeer API Docs</h1>
      </header>

      <nav className="docs__toc">
        <ol>
          <li><a href="#overview">Overview</a></li>
          <li><a href="#api-key">Getting an API Key</a></li>
          <li><a href="#connecting">Connecting</a></li>
          <li><a href="#inbound">Inbound Messages (Server → Client)</a></li>
          <li><a href="#outbound">Outbound Messages (Client → Server)</a></li>
          <li><a href="#sequences">Event Sequences</a></li>
          <li><a href="#bot-patterns">Bot Design Patterns</a></li>
          <li><a href="#market-data">Market Data Feed</a></li>
          <li><a href="#templates">Template Downloads</a></li>
        </ol>
      </nav>

      <main className="docs__body">

        {/* ── 1. Overview ─────────────────────────────────────────────── */}
        <section id="overview">
          <h2>1. Overview</h2>
          <p>
            Anjeer is a real-time card-trading game. Players hold cards across four suits
            and trade them using a continuous double auction (limit orders) during timed rounds.
            The player who holds the most cards of the secret <em>goal suit</em> at round end wins the pot.
          </p>
          <p>
            <strong>API mode</strong> lets you connect a script or bot instead of a human browser.
            Create an <em>API lobby</em>, generate an API key, and connect via WebSocket with a
            Bearer token header. Spectators can watch any API lobby live and read your script&apos;s
            log messages in real time.
          </p>
        </section>

        {/* ── 2. Getting an API Key ───────────────────────────────────── */}
        <section id="api-key">
          <h2>2. Getting an API Key</h2>
          <ol>
            <li>Log in and navigate to <a href="/api-keys">Settings → API Keys</a>.</li>
            <li>Enter a name and click <strong>Generate API Key</strong>.</li>
            <li>Copy the key immediately — it is shown <strong>only once</strong>. The key starts with <code>ank_</code>.</li>
            <li>Keys expire after 30 days. Revoke and regenerate as needed. Only one active key per account is allowed.</li>
          </ol>
        </section>

        {/* ── 3. Connecting ───────────────────────────────────────────── */}
        <section id="connecting">
          <h2>3. Connecting</h2>
          <h3>WebSocket URL</h3>
          <pre><code>{`ws://<host>/ws`}</code></pre>
          <h3>Authentication</h3>
          <p>Pass your API key in the HTTP upgrade request:</p>
          <pre><code>{`Authorization: Bearer ank_<your-key>`}</code></pre>
          <p>
            A query-parameter fallback is also accepted (<code>?api_key=ank_...</code>) but
            triggers a server warning and should not be used in production — the key is visible
            in server logs and browser history.
          </p>
          <h3>Lobby mode enforcement</h3>
          <p>
            API key connections can only join <em>API-mode lobbies</em>. Attempting to join a
            UI-mode lobby will close the connection with <code>LOBBY_MODE_MISMATCH</code>.
            Similarly, browser (JWT) connections cannot join API-mode lobbies.
          </p>
          <h3>Rate limiting</h3>
          <p>
            Each connection is subject to a token-bucket rate limiter (default: 20-token capacity,
            5 tokens/second refill). Exceeding capacity returns a <code>RATE_LIMIT_WARNING</code>;
            sustained excess closes the connection with <code>RATE_LIMIT_EXCEEDED</code>.
          </p>

          <h3>Running via the CLI (<code>anjeer join</code> / <code>anjeer create</code>)</h3>
          <p>
            When you launch a script through the Anjeer CLI, the CLI handles lobby joining before
            your script ever runs. <strong>Your script must not call the join HTTP endpoint.</strong>
            By the time your process starts, the lobby status is already <code>starting</code> or{' '}
            <code>active</code> — a POST to <code>/lobbies/:code/join</code> will return
            <code>409 GAME_ALREADY_STARTED</code>.
          </p>
          <p>Your script&apos;s job starts at WebSocket connect, responding to <code>player_hello</code>.</p>
          <p>The CLI sets the following environment variables before exec&apos;ing your script:</p>
          <pre><code>{`ANJEER_API_KEY          # Bearer token — pass in Authorization header
ANJEER_SERVER_WS_URL    # WebSocket URL, e.g. ws://localhost:9001/ws
ANJEER_HTTP_URL         # HTTP base URL, e.g. http://localhost:10000
ANJEER_LOBBY_CODE       # Lobby code your script is participating in`}</code></pre>
          <p>
            Read these at startup and exit loudly if a required one is missing — the templates
            below handle this correctly. Do not add join HTTP logic on top of them.
          </p>
          <p>
            Python: read with <code>os.environ.get()</code>.
            C++: read with <code>std::getenv()</code>. The C++ template requires
            Boost.Beast ≥ 1.81 and nlohmann/json ≥ 3.11; build with:
          </p>
          <pre><code>{`g++ -std=c++17 -O2 anjeer_template.cpp -lboost_system -lpthread -o anjeer_bot`}</code></pre>
        </section>

        {/* ── 4. Inbound Messages ─────────────────────────────────────── */}
        <section id="inbound">
          <h2>4. Inbound Messages (Server → Client)</h2>
          <p>All messages are JSON objects with a <code>type</code> discriminant.</p>

          <h3><code>player_hello</code></h3>
          <p>Sent once on connect. Confirms your player identity.</p>
          <pre><code>{`{ type: 'player_hello', player_id: number, role?: 'player' | 'spectator' }`}</code></pre>

          <h3><code>book_update</code></h3>
          <p>Broadcast after every order book mutation. One message per suit.</p>
          <pre><code>{`{ type: 'book_update', suit: string,
  best_bid: number | null, best_ask: number | null,
  best_bid_slot: number | null, best_ask_slot: number | null }`}</code></pre>

          <h3><code>trade</code></h3>
          <p>Broadcast when a trade executes. All four books are wiped immediately after.</p>
          <pre><code>{`{ type: 'trade', suit: string, price: number,
  aggressor_side: 'buy' | 'sell',
  your_side: 'buy' | 'sell' | null,
  buyer_slot: number, seller_slot: number }`}</code></pre>

          <h3><code>order_ack</code></h3>
          <p>Private confirmation that your order or nudge was accepted.</p>
          <pre><code>{`{ type: 'order_ack', order_id: number, suit: string, side: 'buy' | 'sell', price: number }`}</code></pre>

          <h3><code>order_cancel_ack</code></h3>
          <p>Private confirmation that your cancel was accepted.</p>
          <pre><code>{`{ type: 'order_cancel_ack', order_id: number }`}</code></pre>

          <h3><code>error</code></h3>
          <p>Sent to the submitting client when a command is rejected. Switch on <code>code</code>, not <code>message</code>.</p>
          <pre><code>{`{ type: 'error', code: string, message: string }
// Stable codes: PRICE_OUT_OF_RANGE | ORDER_NOT_FOUND | NOT_YOUR_ORDER
//   UNKNOWN_SUIT | MALFORMED_MESSAGE | INSUFFICIENT_BALANCE
//   API_KEY_INVALID | LOBBY_MODE_MISMATCH | SPECTATOR_NOT_ALLOWED
//   RATE_LIMIT_WARNING | RATE_LIMIT_EXCEEDED`}</code></pre>

          <h3><code>round_starting</code></h3>
          <p>Broadcast when all players are connected and the countdown begins.</p>
          <pre><code>{`{ type: 'round_starting', starts_at: string /* ISO 8601 UTC */, player_count: number }`}</code></pre>

          <h3><code>round_start</code></h3>
          <p>Sent privately to each player when the round opens. Contains your hand and the full roster.</p>
          <pre><code>{`{ type: 'round_start', player_slot: number,
  hand: { clubs: number, diamonds: number, hearts: number, spades: number },
  round_end_at: string /* ISO 8601 UTC */,
  balance: number,
  roster: Array<{ player_slot: number, username: string }>,
  all_hand_totals: number[],   // indexed by slot
  all_balances: number[]       // indexed by slot }`}</code></pre>

          <h3><code>round_end</code></h3>
          <p>Sent privately at round end with your payout and standing.</p>
          <pre><code>{`{ type: 'round_end', goal_suit: string,
  results: Array<{ player_slot: number, goal_cards_held: number,
                   payout: number, balance: number, disconnected: boolean }> }`}</code></pre>

          <h3><code>delta_update</code></h3>
          <p>Broadcast after every trade. Full snapshot of net card flow this round.</p>
          <pre><code>{`{ type: 'delta_update', deltas: number[][] }
// deltas[player_slot][suit_index]; suit order: 0=clubs 1=diamonds 2=hearts 3=spades`}</code></pre>

          <h3><code>all_balances</code></h3>
          <p>Broadcast after every trade once balances are settled.</p>
          <pre><code>{`{ type: 'all_balances', balances: number[] }  // indexed by slot`}</code></pre>

          <h3><code>hand_totals</code></h3>
          <p>Broadcast after every trade once card transfers complete.</p>
          <pre><code>{`{ type: 'hand_totals', totals: number[] }  // indexed by slot`}</code></pre>

          <h3><code>inter_round</code></h3>
          <p>Broadcast at round end. Includes standings and the auto-start countdown.</p>
          <pre><code>{`{ type: 'inter_round', round_number: number, goal_suit: string,
  results: PlayerRoundResult[],
  next_round_at: string | null }`}</code></pre>

          <h3><code>game_ended</code></h3>
          <p>Broadcast when the game ends (owner ends it or all players leave).</p>
          <pre><code>{`{ type: 'game_ended',
  rounds: Array<{ round_number, goal_suit, results }>,
  final_standings: Array<{ player_slot, username, final_balance, net_change }> }`}</code></pre>

          <h3><code>game_player_left</code></h3>
          <p>Broadcast when a player disconnects mid-game.</p>
          <pre><code>{`{ type: 'game_player_left', player_slot: number, username: string }`}</code></pre>

          <h3><code>session_error</code></h3>
          <p>Broadcast on unhandled server error. The session tears down after this.</p>
          <pre><code>{`{ type: 'session_error', message: string }`}</code></pre>

          <h3><code>spectator_count</code></h3>
          <p>Broadcast to <em>players</em> (not spectators) when the spectator count changes.</p>
          <pre><code>{`{ type: 'spectator_count', count: number }`}</code></pre>

          <h3><code>script_log</code></h3>
          <p>Forwarded to <em>spectators only</em> when an API-lobby player sends a script_log command. Control characters stripped, truncated to 500 chars.</p>
          <pre><code>{`{ type: 'script_log', player_slot: number, message: string, timestamp: number }`}</code></pre>

          <h3><code>heartbeat</code></h3>
          <p>Sent by the server at a fixed interval. No response required — safe to ignore.</p>
          <pre><code>{`{ type: 'heartbeat', server_time: string /* ISO 8601 UTC */ }`}</code></pre>

          <h3><code>waiting_for_start</code></h3>
          <p>Broadcast while the lobby is waiting for enough players to connect before the countdown begins.</p>
          <pre><code>{`{ type: 'waiting_for_start', connected: number, required: number }`}</code></pre>
        </section>

        {/* ── 5. Outbound Messages ────────────────────────────────────── */}
        <section id="outbound">
          <h2>5. Outbound Messages (Client → Server)</h2>

          <h3><code>submit_order</code></h3>
          <pre><code>{`{ type: 'submit_order', suit: 'clubs'|'diamonds'|'hearts'|'spades', side: 'buy'|'sell', price: number }`}</code></pre>

          <h3><code>nudge</code></h3>
          <p>Adjusts your best standing order by ±1 tick toward the spread.</p>
          <pre><code>{`{ type: 'nudge', suit: string, side: 'buy'|'sell' }`}</code></pre>

          <h3><code>cancel_order</code></h3>
          <pre><code>{`{ type: 'cancel_order', order_id: number }`}</code></pre>

          <h3><code>end_game</code></h3>
          <p>Owner only. Ends the game immediately during the inter-round window.</p>
          <pre><code>{`{ type: 'end_game' }`}</code></pre>

          <h3><code>start_next_round</code></h3>
          <p>Owner only. Starts the next round early, bypassing the auto-start countdown.</p>
          <pre><code>{`{ type: 'start_next_round' }`}</code></pre>

          <h3><code>script_log</code></h3>
          <p>API-lobby players only. Forwarded (sanitized, truncated) to all spectators.</p>
          <pre><code>{`{ type: 'script_log', message: string }`}</code></pre>

          <h3><code>subscribe_lobby</code> / <code>unsubscribe_lobby</code></h3>
          <p>Subscribe to lobby-layer events (player_joined, player_left, lobby_started). Used by the lobby browser UI.</p>
          <pre><code>{`{ type: 'subscribe_lobby',   lobby_id: string }
{ type: 'unsubscribe_lobby', lobby_id: string }`}</code></pre>

          <h3><code>leave_lobby</code></h3>
          <p>Send before navigating away from a lobby room to clean up server-side state immediately.</p>
          <pre><code>{`{ type: 'leave_lobby', lobby_id: string }`}</code></pre>

          <h3><code>spectate_lobby</code></h3>
          <p>Send on connect to enter a session as a read-only spectator.</p>
          <pre><code>{`{ type: 'spectate_lobby', lobby_id: string }`}</code></pre>
        </section>

        {/* ── 6. Event Sequences ──────────────────────────────────────── */}
        <section id="sequences">
          <h2>6. Event Sequences</h2>

          <h3>Round lifecycle</h3>
          <ol>
            <li><strong>All players connect</strong> → server broadcasts <code>round_starting</code> with <code>starts_at</code> timestamp.</li>
            <li><strong>Countdown expires</strong> → server sends <code>round_start</code> privately to each player (your hand, round end time, roster).</li>
            <li><strong>Trading phase</strong> → players submit orders; each mutation emits <code>book_update</code>. Trades emit <code>trade</code> + <code>delta_update</code> + <code>all_balances</code> + <code>hand_totals</code>. All four books wipe on every trade.</li>
            <li><strong>Round end</strong> → server sends <code>round_end</code> privately, then broadcasts <code>inter_round</code> with standings and auto-start countdown.</li>
            <li><strong>Inter-round</strong> → lobby owner may send <code>start_next_round</code> or <code>end_game</code>; countdown auto-starts the next round when it expires.</li>
            <li><strong>Game over</strong> → server broadcasts <code>game_ended</code> with full history.</li>
          </ol>

          <h3>Order lifecycle</h3>
          <ol>
            <li>Send <code>submit_order</code>.</li>
            <li>Server validates and responds with either <code>order_ack</code> (accepted) or <code>error</code> (rejected).</li>
            <li>If the order rests in the book, a <code>book_update</code> is broadcast to all players.</li>
            <li>If the order crosses an existing resting order, a <code>trade</code> fires, both sides receive <code>your_side</code>, and all four books wipe (<code>book_update ×4</code>).</li>
            <li>To cancel a resting order, send <code>cancel_order</code> with the <code>order_id</code> from <code>order_ack</code>. Server responds with <code>order_cancel_ack</code> or <code>error</code>.</li>
          </ol>
        </section>

        {/* ── 7. Bot Design Patterns ──────────────────────────────────── */}
        <section id="bot-patterns">
          <h2>7. Bot Design Patterns</h2>
          <p>
            The message schemas above describe individual events. This section describes how
            to compose them into a working bot — covering the state you need to track, the
            signals worth acting on, and the pitfalls that aren&apos;t obvious from the schema alone.
          </p>

          <h3>The wipe mechanic</h3>
          <p>
            <strong>Every trade wipes all four order books.</strong> After any <code>trade</code> event,
            every resting order across all suits is cancelled — including yours. Your bot must
            re-post orders after every trade, not just at round start. A bot that posts once and
            waits will go idle after the first trade fires.
          </p>
          <p>
            This also means you never need to explicitly cancel a resting order after a trade —
            the wipe has already done it. Sending a <code>cancel_order</code> for a wiped order
            returns <code>ORDER_NOT_FOUND</code>.
          </p>

          <h3>Pending order state</h3>
          <p>Track your resting order as a single object updated by these four events:</p>
          <pre><code>{`// On order_ack → store the full order
pending = { order_id, suit, side, price }

// On order_cancel_ack → clear it
pending = null

// On trade (any) → clear it — the wipe removed your order regardless of your_side
pending = null

// On round_end / inter_round → clear it
pending = null`}</code></pre>
          <p>
            The key mistake is clearing <code>pending</code> only when <code>your_side</code> is set on a
            trade. A trade by <em>anyone</em> wipes your order. If you only clear on fills, you&apos;ll
            loop sending cancels for an order that no longer exists.
          </p>

          <h3>Hand tracking</h3>
          <p>Your hand is not re-broadcast after each trade — you must maintain it yourself:</p>
          <pre><code>{`// On round_start → set initial hand
hand = msg.hand  // { clubs, diamonds, hearts, spades }

// On trade where your_side is set → update
if (msg.your_side === 'buy')  hand[msg.suit] += 1
if (msg.your_side === 'sell') hand[msg.suit] -= 1`}</code></pre>

          <h3>Mid-price construction</h3>
          <p>
            After a wipe, both sides of a book may be empty. Rather than hardcoding a fallback
            price, maintain a last-trade-price per suit and derive a mid from what&apos;s available:
          </p>
          <pre><code>{`function midPrice(suit) {
  const { best_bid, best_ask } = books[suit]
  if (best_bid != null && best_ask != null) return (best_bid + best_ask) / 2
  if (best_ask != null) return best_ask
  if (best_bid != null) return best_bid
  if (lastTradePrice[suit] != null) return lastTradePrice[suit]
  return pointsPerCard  // cold fallback
}

// Update lastTradePrice on every trade event
lastTradePrice[msg.suit] = msg.price`}</code></pre>
          <p>
            Use <code>midPrice(suit)</code> to anchor passive bids (e.g. <code>midPrice * 0.9</code>) and
            passive offers (e.g. <code>midPrice * 1.1</code>). To trade aggressively, lift the ask or
            hit the bid directly.
          </p>

          <h3><code>delta_update</code> as the primary alpha signal</h3>
          <p>
            <code>delta_update</code> broadcasts a cumulative snapshot of net card flow per player per
            suit since round start. It is the strongest public signal for inferring the goal suit —
            sustained net buying of suit S by multiple opponents is evidence S is likely the goal.
          </p>
          <pre><code>{`// deltas[player_slot][suit_index]
// suit indices: 0=clubs 1=diamonds 2=hearts 3=spades

// Diff successive snapshots to get the per-trade increment
const inc = deltas[p][s] - prevDeltas[p][s]
// inc > 0 → player p bought a card of suit s this trade
// inc < 0 → player p sold`}</code></pre>
          <p>
            A simple signal: sum increments across all opponents for each suit after each trade.
            The suit with the most sustained opponent buying is the highest-conviction goal candidate.
            Weight recent increments more heavily than early-round ones.
          </p>

          <h3>Recommended event loop structure</h3>
          <pre><code>{`on_message(msg):
  update_book_state(msg)       // book_update, trade → lastTradePrice
  update_hand(msg)             // trade where your_side is set
  update_pending(msg)          // order_ack, cancel_ack, any trade
  update_alpha(msg)            // delta_update → belief/signal update
  if should_act():             // round active, no pending order
    action = decide()          // buy / sell / hold
    send(action)`}</code></pre>
          <p>
            The key constraint: <strong>check for a pending order before acting</strong>. Submitting
            while one is resting adds a second resting order, which is fine, but you lose track of
            which cancel belongs to which order. The simplest correct model is one resting order at a time.
          </p>
        </section>

        {/* ── 8. Market Data Feed ─────────────────────────────────────── */}
        <section id="market-data">
          <h2>8. Market Data Feed</h2>
          <p>
            Anjeer exposes three tiers of market data detail. Each tier is a strict superset
            of the previous — MBO contains everything MBP-N contains, which contains everything
            MBP-1 contains, plus additional fields.
          </p>

          <h3>Three-tier model</h3>
          <table className="docs__table">
            <thead>
              <tr><th>Tier</th><th>What you see</th><th>Use case</th></tr>
            </thead>
            <tbody>
              <tr>
                <td><code>mbp1</code> (default)</td>
                <td>Best bid and ask per suit</td>
                <td>Simple bots; minimal bandwidth</td>
              </tr>
              <tr>
                <td><code>mbpn</code></td>
                <td>Full price-level depth (price → total resting qty)</td>
                <td>Depth-of-book signals; queue-position estimation</td>
              </tr>
              <tr>
                <td><code>mbo</code></td>
                <td>Individual orders (order_id, side, price)</td>
                <td>Full local-book reconstruction; cross-player order attribution</td>
              </tr>
            </tbody>
          </table>

          <h3>Delivery endpoints</h3>
          <table className="docs__table">
            <thead>
              <tr><th>Endpoint</th><th>Tier delivered</th><th>Auth</th></tr>
            </thead>
            <tbody>
              <tr>
                <td><code>/ws</code></td>
                <td>Tier from your <code>feed_preference</code> (default <code>mbp1</code>)</td>
                <td>Bearer JWT or API key</td>
              </tr>
              <tr>
                <td><code>/ws/marketdata</code></td>
                <td>Always <code>mbo</code>, regardless of <code>feed_preference</code></td>
                <td>Bearer JWT or API key</td>
              </tr>
            </tbody>
          </table>

          <h3>Setting your feed preference</h3>
          <p>Call <code>PUT /players/me/feed</code> or use the CLI:</p>
          <pre><code>{`# REST
curl -X PUT http://localhost:10000/players/me/feed \\
  -H "Authorization: Bearer ank_<key>" \\
  -H "Content-Type: application/json" \\
  -d '{"feed_preference": "mbpn"}'

# CLI
anjeer feed mbp-n   # normalised to mbpn
anjeer feed mbo`}</code></pre>
          <p>The preference is read at WS upgrade time — reconnect after changing it.</p>

          <h3>Sequence numbers (<code>seq</code>) and schema version (<code>v</code>)</h3>
          <p>
            Every market-data message carries two numeric fields:
          </p>
          <ul>
            <li><code>v</code> — wire schema version, currently <code>1</code>. Increment is a breaking change.</li>
            <li>
              <code>seq</code> — monotonically increasing integer stamped by the exchange sequencer.
              Starts near <code>1</code> at the beginning of each round and increments by 1 for every
              order-book event (submit, cancel, execute). Resets to <code>0</code> at <code>begin_round</code>.
            </li>
          </ul>
          <p>
            <strong>Snapshot <code>seq</code>:</strong> A snapshot message (sent on connect or on <code>resync</code>)
            carries the <code>seq</code> of the last applied event. The very next incremental message
            will have <code>seq = snapshot_seq + 1</code>. Use this to verify you have received a
            contiguous stream.
          </p>
          <pre><code>{`// Gap detection
let expectedSeq = null

on_message(msg):
  if (msg.seq != null):
    if (isSnapshot(msg)):
      expectedSeq = msg.seq + 1        // next incremental will be this
    else:
      if (expectedSeq != null && msg.seq !== expectedSeq):
        // gap detected — send resync to recover
        sendResync()
        expectedSeq = null
      else:
        expectedSeq = msg.seq + 1`}</code></pre>

          <h3>Gap recovery — <code>resync</code></h3>
          <p>
            If you detect a sequence gap (or simply want a clean state), send a <code>resync</code>
            message. The server responds with one snapshot per instrument matching your current tier:
          </p>
          <pre><code>{`// Send from client
{ "type": "resync" }

// MBP-1 response: one book_update per instrument
// MBP-N response: one book_depth_snapshot per instrument
// MBO response:   one order_book_snapshot per instrument`}</code></pre>

          <h3>MBP-1 messages</h3>
          <p><code>book_update</code> — emitted after every order-book mutation:</p>
          <pre><code>{`{
  type: "book_update", v: 1, seq: number,
  suit: string,
  best_bid: number | null,
  best_ask: number | null
}`}</code></pre>

          <h3>MBP-N messages</h3>
          <p><code>book_depth_snapshot</code> — sent on connect/resync, one per instrument:</p>
          <pre><code>{`{
  type: "book_depth_snapshot", v: 1, seq: number, suit: string,
  bids: [{ price: number, qty: number }, ...],  // descending price
  asks: [{ price: number, qty: number }, ...]   // ascending price
}`}</code></pre>
          <p><code>book_depth</code> — incremental update after every order-book event:</p>
          <pre><code>{`{
  type: "book_depth", v: 1, seq: number, suit: string,
  bids: [{ price: number, qty: number }, ...],
  asks: [{ price: number, qty: number }, ...]
}`}</code></pre>
          <p>
            Each <code>book_depth</code> is a <strong>full replacement snapshot</strong> of the current depth, not a delta.
            Apply it by replacing your local depth map for the named suit outright.
          </p>

          <h3>MBO messages</h3>
          <p><code>order_book_snapshot</code> — sent on connect/resync, one per instrument:</p>
          <pre><code>{`{
  type: "order_book_snapshot", v: 1, seq: number, suit: string,
  bids: [{ order_id: number, price: number }, ...],
  asks: [{ order_id: number, price: number }, ...]
}`}</code></pre>
          <p><code>order_added</code> — new resting order:</p>
          <pre><code>{`{ type: "order_added", v: 1, seq: number,
  order_id: number, suit: string, side: "buy"|"sell", price: number }`}</code></pre>
          <p><code>order_executed</code> — order matched (both sides removed from book):</p>
          <pre><code>{`{ type: "order_executed", v: 1, seq: number,
  order_id: number, suit: string, price: number,
  aggressor_side: "buy"|"sell",
  buyer_slot: number, seller_slot: number }`}</code></pre>
          <p><code>order_cancelled</code> — resting order explicitly cancelled or wiped:</p>
          <pre><code>{`{ type: "order_cancelled", v: 1, seq: number,
  order_id: number, suit: string }`}</code></pre>

          <h3>MBO local book reconstruction</h3>
          <p>
            Maintain a <code>Map&lt;order_id, Order&gt;</code> per suit. Apply events in <code>seq</code> order:
          </p>
          <pre><code>{`// State
const localBook = {
  clubs:    { bids: new Map(), asks: new Map() },
  diamonds: { bids: new Map(), asks: new Map() },
  hearts:   { bids: new Map(), asks: new Map() },
  spades:   { bids: new Map(), asks: new Map() },
}

function applyMboEvent(msg) {
  const book = localBook[msg.suit]
  if (!book) return

  if (msg.type === "order_book_snapshot") {
    book.bids.clear()
    book.asks.clear()
    for (const o of msg.bids) book.bids.set(o.order_id, o)
    for (const o of msg.asks) book.asks.set(o.order_id, o)
    return
  }
  if (msg.type === "order_added") {
    const side = msg.side === "buy" ? book.bids : book.asks
    side.set(msg.order_id, { order_id: msg.order_id, price: msg.price })
    return
  }
  if (msg.type === "order_executed" || msg.type === "order_cancelled") {
    // The wipe-on-trade mechanic means order_executed is followed by
    // order_cancelled events for every remaining resting order.
    // Removing by order_id handles both the matched order and wipe events.
    book.bids.delete(msg.order_id)
    book.asks.delete(msg.order_id)
    return
  }
}

// Best bid/ask from local book
function bestBid(suit) {
  return [...localBook[suit].bids.values()]
    .reduce((best, o) => (!best || o.price > best.price ? o : best), null)
}
function bestAsk(suit) {
  return [...localBook[suit].asks.values()]
    .reduce((best, o) => (!best || o.price < best.price ? o : best), null)
}`}</code></pre>
          <p>
            <strong>Wipe mechanic note:</strong> Every trade wipes all four books. After an{' '}
            <code>order_executed</code> event you will receive <code>order_cancelled</code> for every
            remaining resting order. Your reconstruction loop handles this correctly via the{' '}
            <code>delete</code> branch — no special wipe-detection code needed.
          </p>
        </section>

        {/* ── 9. Template Downloads ───────────────────────────────────── */}
        <section id="templates">
          <h2>9. Template Downloads</h2>
          <p>
            These templates handle connection, authentication, and all message types.
            They are designed to be launched via <code>anjeer join</code> or <code>anjeer create</code> —
            the CLI sets all required environment variables and owns the lobby join step.
            Do not add join HTTP calls to the template.
          </p>
          <div className="docs__templates">
            <a
              className="docs__template-link"
              href="/examples/anjeer_template.py"
              download="anjeer_template.py"
            >
              Download Python template
              <span className="docs__template-meta">anjeer_template.py · websockets + asyncio</span>
            </a>
            <a
              className="docs__template-link"
              href="/examples/anjeer_template.cpp"
              download="anjeer_template.cpp"
            >
              Download C++ template
              <span className="docs__template-meta">anjeer_template.cpp · Boost.Beast async</span>
            </a>
          </div>
        </section>


      </main>
    </div>
  )
}
