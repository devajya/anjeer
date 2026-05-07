# Anjeer — Architecture

> Abstraction-level: pipelines and module responsibilities. Not function names or field names.
> Update this doc when a new module is added, a pipeline stage changes, or the wire protocol gains a new message category.
> Do NOT update for internal refactors, renamed methods, or implementation changes within an existing module.

## System Overview

Three-layer architecture. Strict dependency direction: Frontend → (WebSocket) → Server → Engine. No layer reaches backward.

```
┌─────────────────────────────────┐
│  Frontend  (React + TypeScript) │  Browser, port 5173 (dev) / CDN (prod)
└──────┬───────────────┬──────────┘
       │ WebSocket /ws │ HTTP /auth /players /api
       │               │
┌──────▼──────┐  ┌─────▼────────────────────────┐
│  WsServer   │  │  HttpServer  (Crow async)     │
│  uWS :9001  │  │  :8080                        │
│  game loop  │  │  OAuth, JWT, REST endpoints   │
└──────┬──────┘  └─────┬────────────────────────┘
       │               │
       │    shared: DbPool, ServerConfig
       │               │
       │         ┌─────▼─────────┐
       │         │  PostgreSQL   │  port 5432 (local dev) / Neon / RDS (prod)
       │         └───────────────┘
       │
┌──────▼──────────────────────────┐
│  Engine    (C++ / pure logic)   │  Static library, no I/O
│  Owns: matching, game state,    │
│  scoring, all game rules        │
└─────────────────────────────────┘
```

## Engine Layer

**Role:** Pure game logic. No network, no JSON, no file I/O. Returns structured events; callers own dispatch.

**Current modules (Slice 8):**
- `OrderBook` — price-time priority limit order matching. Operations: submit, nudge (best±1), cancel, wipe (global clear). Returns a vector of typed events per operation. `BookUpdateEvent` carries `best_bid_player_id` / `best_ask_player_id` (slot indices) so the frontend can colour quote owners.
- `GameState` — deals deck, tracks per-player hand counts. Goal suit is **explicit per-deck** (passed in `Config::goal_suit`); it is no longer derived from the distribution. `transfer_card` mutates hand state after each trade for accurate end-of-round scoring. `suit.h` provides the `Suit` enum and color helpers reused by future modules.
- `ScoringEngine` — pure `score_round()` function. Takes per-player hands, goal suit, disconnected flags, and `ScoringConfig`. `ScoringConfig::bonus_pool` is explicit (deck-supplied); it is no longer computed from the distribution. Computes pot, per-card payout, majority/plurality bonus, and payouts. No I/O; server owns dispatch.

**Planned modules (future slices):**
- `BotAgent` (Slice 10) — strategy implementations connecting via the same WS interface as human players
- `EvalModule` / `EvalRunner` (Slice 11) — plugin-style analysis running on a separate thread, read-only game state snapshots
- `ReplayEngine` (Slice 13) — deterministic state reconstruction from event log

**Event model:** Every engine operation returns `std::vector<OrderEvent>` (a `std::variant` of typed event structs). Server iterates the vector and serializes each event to the appropriate WebSocket recipients. Engine never decides who receives what.

## Server Layer

**Role:** Owns the WebSocket connection lifecycle, JSON serialization, config, logging, and game loop. Translates between wire protocol and engine calls.

**Current responsibilities (Slice 7):**

`WsServer` (uWS, port 9001):
- Thin dispatch layer: parses inbound JSON, enqueues `NetEvent` onto the active session's inbound SPSC queue; drains each session's outbound `GameEvent` queue every 16ms via uWS timer
- Maintains `unordered_map<lobby_id, ActiveSession>` — each entry holds the two SPSC queues, a `GameSession` instance, and a slot↔WsHandle map
- `subscribe_lobby` / `unsubscribe_lobby` — per-connection lobby subscriptions backed by `IEventBus`; cleanup on close
- `leave_lobby` — removes player from `lobby_players`, calls `delete_if_empty`, tears down session if empty
- HTTP POST `/api/log` — frontend log batches → `logs/frontend_logs.txt`

`GameSession` (Slice 7+):
- Runs on a dedicated game-loop thread; communicates with WsServer exclusively via two lock-free SPSC queues (`session_queue.h`: `NetEvent` inbound, `GameEvent` outbound)
- `SessionPhase` state machine: `Lobby → Countdown → RoundActive → InterRound → Ended`
- Deck selection: 12 pre-defined `DeckDef` entries in a static `kDecks` table. Each deck specifies per-suit distribution, explicit goal suit, and bonus pool. One deck is picked uniformly at `begin_round` and stored as `current_deck_` — the single authoritative source for all per-round deck properties.
- Post-trade pipeline (`apply_post_trade_state`): card transfers → balance settlements → delta accumulation → `delta_update` broadcast. Called only when a trade occurred; `apply_global_wipe` follows.
- Delta table: 4×4 `delta_table_[player_slot][suit_index]` — net cards gained this round, broadcast as a full snapshot after each trade.
- Collects buy-ins at round start; runs multi-round loop until majority vote or all players leave
- Timers via `std::chrono::steady_clock` on the game thread — no uWS involvement
- Persists session + round records to DB via `SessionRepo`; writes `session_errors` on unhandled exceptions

`HttpServer` (Crow async, port 8080):
- `GET /auth/{provider}` — redirect to OAuth provider with CSRF state nonce
- `GET /auth/{provider}/callback` — exchange code → `AuthService::find_or_create` → JWT cookies → redirect frontend
- `POST /auth/refresh` — validate refresh token cookie → issue new access token cookie
- `POST /auth/logout` — clear cookies
- `GET /players/me` — JWT middleware → `PlayerRepo::find_by_id` → profile JSON
- `POST /lobbies` — create lobby; inserts creator row; returns 201 LobbyView JSON
- `GET /lobbies` — list waiting + active lobbies with player counts
- `POST /lobbies/:id/join` — add authenticated player; 409 on full/not-waiting
- `POST /lobbies/:id/start` — owner-only; transitions status `waiting → starting`; publishes `lobby_started` to `IEventBus`
- `GET /players/me/keybinds` — returns array of `{action, key_combo}` overrides for the authenticated player
- `PUT /players/me/keybinds` — full-replace keybind overrides via `KeybindsRepo`; server stores only explicit overrides, frontend supplies defaults
- `GET /players/me/api-keys` — list active API keys (name, created_at, expires_at; hash never returned)
- `POST /players/me/api-keys` — generate a new API key; plaintext shown once in response, hash stored
- `DELETE /players/me/api-keys/:id` — revoke a key by setting `revoked_at`
- `GET /lobbies/:id_or_code` — fetch single lobby by UUID or 6-char code
- `POST /players/me/spectate-token` — issue a short-lived single-use `stk_` token for spectator handoff
- `GET /auth/spectate` — consume a spectate token, establish a browser session, redirect to spectator view

`SessionRepo` (Slice 7):
- Writes session lifecycle records to `game_sessions`, `rounds`, `session_errors` tables

`LobbyRepo` (Slice 6+):
- CRUD over `lobbies` + `lobby_players` tables; CAS-style `transition_status`; unique 6-char code generation via OpenSSL; `list_active()` for in-progress lobbies; `delete_if_empty()` for cleanup on leave

`IEventBus` / `LocalEventBus` (Slice 6):
- In-process publish/subscribe over named channels; synchronous delivery on publish thread
- `RedisEventBus` stub present; activated in Slice 16 (Upstash Redis PUBLISH/SUBSCRIBE)

`DbPool` + `DbMigrator` (shared):
- libpqxx connection pool (configurable size); RAII acquire handle
- `DbMigrator::run()` on server start — applies unapplied SQL files from `db/migrations/` in version order

**Slice 9 additions:**
- `ApiKeyRepo` — API key CRUD with SHA-256 hashing; Bearer auth on WS upgrade (query-param fallback with warning); `api_key_invalid` error + close on expiry/revoke
- `RateLimiter` — in-process token bucket per connection; `rate_limit_warning` event; temporary suspension
- `SpectateTokenRepo` — single-use `stk_` tokens for browser spectator handoff
- `lobbies.mode` (`ui`/`api`) enforced at join; `LOBBY_MODE_MISMATCH` error for cross-mode attempts
- Spectator pipeline: `spectate_lobby` handler → `NetSpectatorJoin` → `GameSession` state snapshot; `ActiveSession::spectator_handles_`; `GameBroadcast` drained to spectators; `script_log` WsServer passthrough (sanitised, forwarded directly to spectator handles)

**Planned additions:**
- DB writer thread + async event queue (Slice 12)
- Background job queue for analysis (Slice 14)
- `RedisEventBus` (Slice 16) — swap `LocalEventBus` for multi-node deployments

**Threading model (Slices 1-6):** Single uWS event loop thread. No locks. Engine calls happen synchronously on the event loop. Heartbeat runs on a second thread but does not touch engine state.

**Threading model (Slice 7+):** Each active game session runs on a dedicated `GameSession` thread. The uWS event loop thread and the game-loop thread share no mutable state — all communication passes through two lock-free SPSC queues (`moodycamel::ReaderWriterQueue`): `NetEvent` inbound (network → game) and `GameEvent` outbound (game → network). The network thread enqueues on receive; a 16ms uWS timer drains the outbound queue and dispatches to WebSocket handles.

## Frontend Layer

**Role:** React UI. WebSocket hook owns connection state and message dispatch. Components are stateless consumers of props.

**Component tree:**
```
App
├── /login               → Login
├── /lobby               → LobbyBrowser      — list waiting + active lobbies; create; join by code
├── /lobby/:code         → LobbyRoom         — player roster; WS lobby_state/player_joined/left;
│                                               Start button (owner + count ≥ min_players only);
│                                               emits leave_lobby on back-nav
├── /settings/keybinds   → KeybindSettings   — keybind customisation; GET/PUT /players/me/keybinds
└── /game                → Game              — reads lobby_id from ?lobby_id= query param (Slice 6+)
                              ├── GameEndScreen      — full-screen on game_ended: final standings + per-round breakdown
                              ├── SessionError       — full-screen on session_error: message + return button
                              ├── InterRoundScreen   — overlay on inter_round: standings + vote-to-end + countdown
                              │    └── VoteTally     — live vote count display
                              ├── ShortcutHelp       — shortcut reference overlay (toggle_shortcuts action)
                              ├── ConnectionBanner   — connection status indicator
                              ├── RoundCountdown     — pre-deal countdown; MM:SS timer during round (red in final 30 s)
                              ├── MarketOverview     — own hand counts + DeltaTable for all players
                              │    └── DeltaTable    — per-player per-suit net card flow this round
                              ├── SuitPanel (×N)     — per-suit order form + best bid/ask display (with quote-owner colour)
                              ├── MyOrders           — resting orders list + cancel buttons
                              ├── TradeFeed          — recent trade history with player names, newest first
                              └── RoundEndModal      — goal suit reveal, standings, payout + new balance; dismiss on click
```

**Hooks:**
- `useWebSocket` — single WS connection, message type dispatch, all game state; handles `lobby_state`, `player_joined`, `player_left`, `lobby_started` in addition to game messages
- `useKeyBinds` — fetches per-player keybind overrides from `/players/me/keybinds`; merges with `DEFAULT_BINDS`; exposes stable `binds` map and `update()` method
- `useKeyboardShortcuts` — attaches global `keydown` listener; dispatches 11 trading actions via inverted combo map; disabled during modal overlays
- `useOrderForm` — form state for order submission

**Wire protocol types:** `frontend/src/types/messages.ts` — discriminated unions, single source of truth. Any protocol change must update this file.

## Data Flow: Order Submission

```
User fills order form
  → useOrderForm state update
  → useWebSocket.sendMessage({ type: 'submit_order', suit, side, price })
  → JSON over WebSocket to server

Server receives message
  → JSON parse → type dispatch → handle_submit
  → parse::submit_order() validates required fields
  → validate_suit_and_side() checks suit exists in active books + side is "buy"/"sell"
  → book.submit(player_id, side, price) → returns vector<OrderEvent>
  → dispatch_events() iterates the vector:
      OrderAckEvent    → send to submitting player only
      TradeEvent       → broadcast to ALL, but your_side field is personalized per recipient
                         (buyer gets "buy", seller gets "sell", everyone else gets null)
      BookUpdateEvent  → broadcast to all ONLY if no trade occurred in this batch;
                         if a trade occurred, this event is suppressed (wipe overrides it)
      OrderErrorEvent  → send to submitting player only
  → if dispatch_events() returned had_trade=true:
      apply_post_trade_state() runs in order:
        apply_card_transfers()     → broadcasts hand_totals[] to all
        apply_trade_settlements()  → broadcasts all_balances[] to all
        delta accumulation         → updates delta_table_[buyer][suit]++ / [seller]--
        broadcast_delta_update()   → broadcasts delta_update (full 4×4 snapshot) to all
      apply_global_wipe() calls wipe() on EVERY active book (not just the matched suit)
        → each wipe() returns BookUpdateEvent(suit, null, null)
        → each is broadcast to all connected clients

Frontend on receiving messages:
  order_ack          → adds entry to myOrders[]
  trade              → prepends to trades[], clears myOrders[] entirely
                       (client mirrors the global wipe: any trade = all orders gone)
  book_update        → updates books[suit] record; null bid/ask renders as empty
  hand_totals        → updates allHandTotals[] (total cards per slot)
  all_balances       → updates allBalances[] and derives own balance from allBalances[playerSlot]
  delta_update       → replaces deltas[][] snapshot (full replace, never accumulated)
  error              → stored under the suit key of the last sent command (or '_' for cancel)
```

**Cancel is different:** `cancel_order` carries no `suit` field on the wire. The server
iterates all active books until it finds the order ID, then calls `dispatch_events` on
that book's result. Same wipe logic applies if the cancel somehow triggered a trade
(currently impossible but the code handles it uniformly).

## Data Flow: Round Start (Slice 3+)

```
N connections reach the server (N = game.player_count in config)
  → server broadcasts round_starting { starts_at, player_count } to all
  → clients display a countdown derived from starts_at
  → server sleeps countdown_seconds, then defers to the event-loop thread:

  GameSession picks a DeckDef from the static kDecks table (12 variants, uniform random)
    → deck specifies: per-suit distribution, explicit goal_suit, bonus_pool

  GameState.deal(rng)
    → distributes cards per deck's distribution, round-robin to player slots (one slot gets remainder if uneven)
    → goal_suit is taken directly from the deck — not derived from the distribution

  Server routes:
    per-player round_start { player_slot, hand, round_end_at } → sent to that player's WS only
    goal_suit → withheld until round end (only in engine log)
    suit_totals → withheld (Slice 3 resolution 6: suit counts are private)

  Server starts round expiry timer (round_duration_seconds). See Data Flow: Round End.
```

## Data Flow: Round End (Slice 4+)

```
Round expiry timer fires
  → server transitions RoundPhase: Active → Scoring
  → apply_global_wipe() — all active books wiped; null book_update broadcast to all

  score_round(hands, goal_suit, disconnected, ScoringConfig)
    → pot      = player_count × buy_in
    → per-card = points_per_card × goal_cards_held (per player)
    → bonus_pool = ScoringConfig::bonus_pool (explicit from the deck — not derived)
    → majority threshold = total_goal_cards / 2 + 1 (strict)
        if exactly one player holds ≥ threshold → they receive full bonus_pool
        else → bonus split evenly among player(s) holding the most goal cards (integer division)
    → new_balance = balance_before_buyin − buy_in + payout
    → disconnected players: payout computed and marked, but not delivered (held for future reconnect)

  Server routes:
    per-player round_end { goal_suit, results[] } → sent to each connected slot's WS only
    disconnected slots → skipped (round_end not sent; balance updated in server state)

  → RoundPhase transitions to Ended; new orders rejected with ROUND_NOT_ACTIVE
```

## Wire Protocol Summary

All messages are JSON objects with a `type` string discriminator.

**Server → Client categories:**
| Category | Messages |
|---|---|
| Connection | `heartbeat`, `player_hello` |
| Order lifecycle | `order_ack`, `order_cancel_ack`, `error` |
| Market data | `book_update` |
| Trade | `trade` |
| Round (Slice 3+) | `round_starting`, `round_start` (includes `round_end_at`) |
| Round end (Slice 4+) | `round_end` |
| Auth (Slice 5+) | `waiting_for_start` (WS broadcast); auth via HTTP cookies, not WS |
| Lobby (Slice 6+) | `lobby_state` (snapshot on subscribe), `player_joined`, `player_left`, `lobby_started` |
| Game lifecycle (Slice 7+) | `inter_round`, `vote_tally`, `game_ended`, `game_player_left`, `session_error` |
| Market state (Slice 8+) | `delta_update` (full per-player per-suit net flow snapshot), `all_balances` (all slots after each trade), `hand_totals` (total cards per slot after each trade) |
| API / spectator (Slice 9+) | `api_key_invalid` (key expired or revoked — connection closed after), `rate_limit_warning` (bucket empty), `script_log` (forwarded from script to spectators only) |
| Eval (Slice 11+) | `eval_update` (separate namespace, never mixed with order events) |

**Client → Server categories:**
| Category | Messages |
|---|---|
| Trading | `submit_order`, `nudge`, `cancel_order` |
| Lobby (Slice 6+) | `subscribe_lobby`, `unsubscribe_lobby`, `leave_lobby` |
| Game (Slice 7+) | `vote_to_end` |
| Spectator (Slice 9+) | `spectate_lobby` |
| Script (Slice 9+) | `script_log` (player → server → spectators; plain string, max 500 chars) |
| Subscription (Slice 17+) | `subscribe`, `unsubscribe` |

**Error codes (stable strings):** `PRICE_OUT_OF_RANGE`, `ORDER_NOT_FOUND`, `NOT_YOUR_ORDER`, `UNKNOWN_SUIT`, `MALFORMED_MESSAGE`, `SERVER_FULL`, `ROUND_NOT_ACTIVE`, `NOT_LOBBY_OWNER`, `INSUFFICIENT_PLAYERS`, `GAME_ALREADY_STARTED`, `LOBBY_NOT_FOUND`, `LOBBY_FULL`, `ALREADY_JOINED`, `LOBBY_MODE_MISMATCH`, `SPECTATOR_NOT_ALLOWED`

## Configuration Surface

All tuneable values live in `config/default.json`. Key sections:
- `server` — host, WS port (9001), HTTP port (8080), heartbeat/ping intervals, CORS origin
- `order_book` — price range, nudge seed prices, active suits
- `game` — player count, card distribution, countdown, round duration
- `scoring` — starting balance, buy-in, points per card
- `db` — connection string, pool size, migrations dir
- `auth` — JWT secret, access/refresh TTLs, secure_cookies flag, per-provider OAuth client_id/secret/redirect_uri
- `lobby` — `min_players`, `max_players` (Slice 6+)
- `event_bus` — `"local"` (in-process) or `"redis"` (Upstash, Slice 16+)
- `rate_limit` — `capacity`, `refill_rate`, `suspend_threshold`, `suspend_seconds` (Slice 9+)

## When to Update This Document

This document describes **pipelines, module boundaries, and protocol shape** — not implementation details. Use this table to decide whether a change warrants an update here.

### Always update this doc

| Change | What to update |
|---|---|
| New engine module added (`GameState`, `ScoringEngine`, etc.) | Add to "Engine Layer — Planned/Current modules" list |
| Engine module promoted from planned → current | Move it in the list; update the slice annotation |
| A pipeline stage is added or reordered (e.g. a new step between parse and engine call) | Update the relevant Data Flow diagram |
| New message category added to the wire protocol (e.g. `round_start`, `eval_update`) | Add row to Wire Protocol Summary table |
| Threading model changes (e.g. Slice 6 game loop separation) | Update "Threading model" paragraph in Server Layer |
| New layer added (e.g. a separate eval service, a Redis cache) | Add to System Overview diagram |
| A named invariant in CLAUDE.md changes | Check if it also appears in Key Design Decisions and update both |

### Update this doc if the scope is significant

| Change | Judgement call |
|---|---|
| New server endpoint category (e.g. REST auth routes in Slice 5) | Add to "Server Layer — Planned additions" if it represents a new responsibility |
| New frontend component that handles a new message type | Update Component Tree if it's a first-class UI area, not a sub-widget |
| Config gains a new top-level section | Add to Configuration Surface table |

### Do NOT update this doc

| Change | Why |
|---|---|
| Method renamed or signature changed | Implementation detail; not in this doc |
| New field added to an existing message type | Too granular; `messages.ts` is the source of truth |
| Refactor within a module that doesn't change its external contract | Internal detail |
| Bug fix | No architectural change |
| New test file | No architectural change |
| Logging changes | No architectural change |

### Automation

A git pre-commit hook at `scripts/check-docs-hook.sh` (registered in Makefile) warns
when a new `.h` file is created in `engine/include/` or `server/include/` without a
corresponding change to this file or CLAUDE.md. It does not block the commit — it prints
a reminder. The hook is installed by running `make install-hooks`.

For in-session changes: after adding a new engine or server header, Claude will update
the Module Map in CLAUDE.md and this document's module list before ending the task.
This is specified as a rule in CLAUDE.md's "Documentation Maintenance" section.

---

## Key Design Decisions

| Decision | Rationale |
|---|---|
| Engine returns events, never dispatches | Keeps engine testable in isolation; server decides routing |
| `qty=1` implicit until Slice 17 | Avoids partial-fill complexity before the core game loop is validated |
| `wipe()` after every trade | Game mechanic: resets books after each card changes hands |
| `std::vector` for order book internals | Cache locality at expected scale (≤5 orders/book); not a map/set |
| uWS single event loop thread | Simplest correct model; `GameSession` thread boundary added in Slice 6 when load justifies it |
| Config-driven suits | `active_suits: ["S1"]` in Slice 2, 4 suits in Slice 3+, N instruments in Slice 17 |
