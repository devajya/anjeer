# Anjeer — Architecture

> Abstraction-level: pipelines and module responsibilities. Not function names or field names.
> Update this doc when a new module is added, a pipeline stage changes, or the wire protocol gains a new message category.
> Do NOT update for internal refactors, renamed methods, or implementation changes within an existing module.

## System Overview

Three-layer architecture. Strict dependency direction: Frontend → (WebSocket) → Server → Engine. No layer reaches backward.

```
┌─────────────────────────────────┐
│  Frontend  (React + TypeScript) │  Browser, port 5173 (dev) / CDN (prod)
└────────────────┬────────────────┘
                 │ WebSocket (/ws)  JSON messages, discriminated on `type`
                 │ HTTP (/api)      Logging, auth (future)
┌────────────────▼────────────────┐
│  Server    (C++ / uWebSockets)  │  Port 9001
│  Owns: protocol, serialization, │
│  connection state, game loop    │
└────────────────┬────────────────┘
                 │ C++ function calls (same process, same thread in Slice 2-5)
┌────────────────▼────────────────┐
│  Engine    (C++ / pure logic)   │  Static library, no I/O
│  Owns: matching, game state,    │
│  scoring, all game rules        │
└─────────────────────────────────┘

Database (PostgreSQL, port 5433) — added in Slice 5, accessed by Server only.
```

## Engine Layer

**Role:** Pure game logic. No network, no JSON, no file I/O. Returns structured events; callers own dispatch.

**Current modules (Slice 4):**
- `OrderBook` — price-time priority limit order matching. Operations: submit, nudge (best±1), cancel, wipe (global clear). Returns a vector of typed events per operation.
- `GameState` — deals deck, derives goal suit, tracks per-player hand counts. `transfer_card` mutates hand state after each trade for accurate end-of-round scoring. `suit.h` provides the `Suit` enum and color helpers reused by future modules. Order books are held by the server layer until Slice 6 unifies them here under `GameSession`.
- `ScoringEngine` — pure `score_round()` function. Takes per-player hands, goal suit, pre-buyin balances, and `ScoringConfig`. Computes pot, per-card payout, majority/plurality bonus, and new balances. No I/O; server owns dispatch of `RoundResult`.

**Planned modules (future slices):**
- `RoundTimer` (Slice 4 server-layer) — already implemented as a thread in the server; moves into the engine under `GameSession` in Slice 6
- `GameSession` (Slice 7) — multi-round orchestrator, buy-in collection, end-vote tally; also wraps engine in a thread-safe boundary (Slice 6 threading change)
- `BotAgent` (Slice 10) — strategy implementations connecting via the same WS interface as human players
- `EvalModule` / `EvalRunner` (Slice 11) — plugin-style analysis running on a separate thread, read-only game state snapshots
- `ReplayEngine` (Slice 13) — deterministic state reconstruction from event log

**Event model:** Every engine operation returns `std::vector<OrderEvent>` (a `std::variant` of typed event structs). Server iterates the vector and serializes each event to the appropriate WebSocket recipients. Engine never decides who receives what.

## Server Layer

**Role:** Owns the WebSocket connection lifecycle, JSON serialization, config, logging, and game loop. Translates between wire protocol and engine calls.

**Current responsibilities (Slice 2-4):**
- Accept WS connections; assign player slots; maintain per-slot balance (initialized to `scoring.starting_balance`)
- Deserialize inbound JSON to typed commands
- Call engine; iterate returned events
- Serialize events to JSON; route to correct recipient(s)
- Broadcast book state after every engine operation
- HTTP POST `/api/log` — receives frontend log batches, writes to `logs/frontend_logs.txt`
- Heartbeat loop (configurable interval)
- `RoundPhase` state machine (`Waiting → Active → Scoring → Ended`); gates order commands to Active only (`ROUND_NOT_ACTIVE` otherwise)
- Round expiry timer: on fire → wipe all books → call `score_round()` → deduct buy-in → dispatch per-player `round_end` (skipping disconnected slots) → transition to Ended

**Planned additions:**
- Crow HTTP server for REST endpoints (Slice 5+): auth, lobbies, player stats, replay API
- JWT validation middleware (Slice 5)
- Rate limiter (Slice 9)
- DB writer thread + async event queue (Slice 12)
- Background job queue for analysis (Slice 14)

**Threading model (Slices 1-5):** Single uWS event loop thread. No locks. Engine calls happen synchronously on the event loop. Heartbeat runs on a second thread but does not touch engine state.

**Threading model (Slice 6+):** Engine wrapped in `GameSession`. Network thread and game loop thread communicate via lock-free queue.

## Frontend Layer

**Role:** React UI. WebSocket hook owns connection state and message dispatch. Components are stateless consumers of props.

**Component tree:**
```
App
├── ConnectionBanner       — connection status indicator
├── RoundCountdown         — pre-deal lobby countdown; switches to active-round MM:SS timer after round_start (goes red in final 30 s)
├── SuitPanel (×N)         — per-suit order form + best bid/ask display
├── HandPanel              — per-suit card counts for the local player
├── MyOrders               — resting orders list + cancel buttons
├── TradeFeed              — recent trade history, newest first
└── RoundEndModal          — shown on round_end: goal suit reveal, per-player standings, own payout + new balance; dismiss on click
```

**Hooks:**
- `useWebSocket` — single WS connection, message type dispatch, all game state
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
      apply_global_wipe() calls wipe() on EVERY active book (not just the matched suit)
        → each wipe() returns BookUpdateEvent(suit, null, null)
        → each is broadcast to all connected clients

Frontend on receiving messages:
  order_ack          → adds entry to myOrders[]
  trade              → prepends to trades[], clears myOrders[] entirely
                       (client mirrors the global wipe: any trade = all orders gone)
  book_update        → updates books[suit] record; null bid/ask renders as empty
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

  GameState.deal(rng)
    → shuffles card_distribution across suits
    → derive_goal_suit() — color_partner of the suit with the most cards
    → distributes cards round-robin to player slots (one slot gets remainder if uneven)

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

  score_round(hands, goal_suit, balances, disconnected, ScoringConfig)
    → pot      = player_count × buy_in
    → per-card = points_per_card × goal_cards_held (per player)
    → bonus_pool = pot − (points_per_card × total_goal_cards_in_deck)
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
| Lobby (Slice 6+) | `player_joined`, `player_left`, `lobby_started` |
| Game lifecycle (Slice 7+) | `round_transition`, `game_ended` |
| Eval (Slice 11+) | `eval_update` (separate namespace, never mixed with order events) |

**Client → Server categories:**
| Category | Messages |
|---|---|
| Trading | `submit_order`, `nudge`, `cancel_order` |
| Game (Slice 7+) | `vote_to_end` |
| Subscription (Slice 17+) | `subscribe`, `unsubscribe` |

**Error codes (stable strings):** `PRICE_OUT_OF_RANGE`, `ORDER_NOT_FOUND`, `NOT_YOUR_ORDER`, `UNKNOWN_SUIT`, `MALFORMED_MESSAGE`, `SERVER_FULL`, `ROUND_NOT_ACTIVE`

## Configuration Surface

All tuneable values live in `config/default.json`. Key sections:
- `server` — host, port, heartbeat/ping intervals
- `order_book` — price range, nudge seed prices, active suits
- `game` — player count, card distribution, countdown, round duration
- `scoring` — starting balance, buy-in, points per card
- Future: `lobby` (min players), `rate_limit`

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
