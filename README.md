# Anjeer

A real-time multiplayer card-trading game built from scratch — C++ matching engine, multithreaded game server, probabilistic bot AI, and a React frontend over WebSockets. Everything is production-style: lock-free concurrency, OAuth + JWT auth, a live order book, Bayesian inference bots, and a terminal CLI for scripted players.

> This project is a technical portfolio piece. The sections below are organized by engineering discipline so you can jump to what's relevant to you.

---

## Table of Contents

| Section | Relevant to |
|---|---|
| [System Overview](#system-overview) | Everyone |
| [Matching Engine](#matching-engine) | Systems, Backend, Algorithms |
| [Server & Concurrency](#server--concurrency) | Systems, Backend |
| [Bot AI & Probabilistic Reasoning](#bot-ai--probabilistic-reasoning) | Algorithms, ML, Math |
| [Evaluation Framework](#evaluation-framework) | Algorithms, Data Engineering |
| [Auth, Security & API](#auth-security--api) | Backend, Security |
| [Frontend Architecture](#frontend-architecture) | Frontend, Full-stack |
| [Wire Protocol](#wire-protocol) | Full-stack, Systems |
| [Database & Persistence](#database--persistence) | Backend, Data |
| [CLI & Scripted Players](#cli--scripted-players) | DevTools, Backend |
| [Getting Started](#getting-started) | Everyone |
| [Configuration](#configuration) | Everyone |
| [Makefile Reference](#makefile-reference) | Everyone |
| [Project Layout](#project-layout) | Everyone |

---

## System Overview

```
┌──────────────────────────────────────┐
│  Browser  (React + TypeScript)       │  port 5173 (dev)
└────────┬──────────────────┬──────────┘
         │  WebSocket :9001 │  HTTP :8080
┌────────▼──────┐   ┌───────▼──────────────────┐
│   WsServer    │   │   HttpServer  (Crow)      │
│  uWebSockets  │   │   OAuth · JWT · REST      │
└────────┬──────┘   └───────┬──────────────────┘
         │                  │  shared: DbPool, ServerConfig
         │           ┌──────▼────────┐
         │           │  PostgreSQL   │
         │           └───────────────┘
┌────────▼──────────────────────────────┐
│  Exchange  (market mechanics)         │
│  ExchangeSession · OrderBook          │
│  Sequencer · MarketDataEvent          │
└────────┬──────────────────────────────┘
┌────────▼──────────────────────────────┐
│  Engine  (pure game logic, no I/O)    │
│  GameState · ScoringEngine            │
│  BotAgent (Easy / Medium / Hard)      │
└───────────────────────────────────────┘
```

Four strict layers. Neither the exchange nor the engine has any I/O — both return typed event vectors; the server owns all serialization and dispatch. The frontend is a pure consumer of WebSocket messages.

---

## Matching Engine & Exchange Layer

**Locations:** `engine/` · `exchange/`

The matching stack is split into two pure C++ libraries. Neither has network, filesystem, or JSON dependencies.

### Exchange Layer (`exchange/`)

Sits between the game server and the engine. Owns market mechanics that are independent of game rules.

- **`ExchangeSession`** — owns all four `OrderBook` instances; routes `submit_order` / `cancel_order` / `cancel_player` through a `Sequencer` that stamps monotonic sequence numbers onto every outbound event. Returns an `ExchangeResult{feedback, market}` — `feedback` is the private operational response to the submitting player; `market` is the observable event broadcast to all participants.
- **`Sequencer`** — monotonic `seq_t` counter; `next_seq()` / `reset()`. Guarantees strict event ordering across all instruments in a session.
- **`OrderBook`** — price-time priority limit order matching. Operations: `submit`, `cancel`, `cancel_player`, `wipe`. All prices are `int32_t` — no floats anywhere in the matching path. Multi-quantity orders supported in intermediate and advanced modes.
- `wipe()` fires after every trade in simple and intermediate modes — all four books cleared atomically. Advanced mode disables the wipe, enabling partial fills: a resting order persists with reduced `qty_remaining` after a partial match.

### Engine (`engine/`)

Pure game logic — deck dealing, hand tracking, scoring. No market mechanics.

### Game State

- `GameState` owns per-player hand counts and deck dealing
- 12 pre-defined deck configurations in a static `kDecks` table — each specifies per-suit distribution and an **explicit** goal suit (not derived from counts)
- `transfer_card` mutates hand state post-trade for accurate end-of-round scoring

### Scoring

- `ScoringEngine::score_round()` — pure function, no side effects
- Pot, per-card payouts, majority/plurality bonus split
- `bonus_pool` derived by server as `pot_size − total_goal_cards × points_per_card` and passed in at scoring time — server owns the arithmetic, engine owns the payout logic

---

## Server & Concurrency

**Location:** `server/`

### Threading Model

Four independent threading axes — no shared mutable state, all cross-thread data flows through lock-free SPSC queues (`moodycamel::ReaderWriterQueue`):

```
uWS event loop thread
  ├─ parses inbound JSON → enqueues NetEvent onto session's inbound SPSC queue
  ├─ 16ms drain timer → pops GameEvent from session's outbound SPSC queue → WS send
  ├─ drain_bot_actions() → moves bot action_queue_ items → session inbound SPSC queue
  └─ encoding: JSON text by default; binary msgpack if client negotiated
    Sec-WebSocket-Protocol: anjeer-msgpack (/ws/marketdata always uses msgpack)

GameSession thread (one per active lobby)
  ├─ owns engine calls, round timers, scoring, DB writes
  └─ communicates exclusively via the two SPSC queues above

BotScheduler thread pool (configurable, default 4 threads)
  ├─ tick() → drains event_queue_, updates snapshot, calls decide(), enqueues result
  └─ pending_ delay heap → sim_network_delay_ms models realistic bot latency

EvalRunner worker thread (one per active session)
  ├─ drains 64-slot SPSC queue pushed by the game-loop thread (non-blocking; drops on full)
  ├─ fans each event to BayesianEvalModule, AccumulationEvalModule, ExecutionEvalModule
  └─ emits eval outputs via callback → WsServer routes unicast/broadcast to WS handles
```

### GameSession

- `SessionPhase` state machine: `Lobby → Countdown → RoundActive → InterRound → Ended`
- Post-trade pipeline runs in strict order: card transfers → balance settlements → delta accumulation → broadcast
- Collects buy-ins at round start; multi-round loop until majority vote-to-end or all players leave
- Persists session + round records via `SessionRepo`

### Rate Limiting & Resilience

- In-process token-bucket rate limiter per connection (`rate_limiter.h`) — capacity and refill rate from config
- `rate_limit_warning` event on bucket empty; temporary suspension on sustained excess
- Bot slots use negative `player_id` values; vote majority and min-player checks use `real_player_count_` (bots excluded)

---

## Bot AI & Probabilistic Reasoning

**Location:** `engine/bots/`

Three difficulty tiers, all implementing the `BotAgent` abstract interface. The factory `make_bot()` is the only public entry point.

### Shared Infrastructure

- **`BotAdapter`** bridges the engine strategy to the server's SPSC queue architecture — it drains JSON events from `event_queue_`, maintains a `GameStateSnapshot`, and calls `decide()` on a scheduler thread
- **`BotScheduler`** — fixed thread pool with a min-heap priority queue for scheduled ticks; `tick_jitter_ms` and `thinking_min_ms/max_ms` produce human-like timing distributions

### Easy Bot

Hand-heuristic belief (no Bayesian inference). Scans for taker opportunities when spreads are wide, places gap-fill maker quotes, cancels stale orders via `max_resting_ms`. Simple but fast.

### Medium Bot

- Opens with a multivariate hypergeometric posterior over the 12 deck configurations from the player's starting hand
- Noisy Bayesian update on observed trades (partial information — only suit and price visible, not hands)
- EV-threshold maker/taker decisions: submits if `E[value of card] > ask` (taker) or quotes if spread gap is profitable (maker)
- `conviction_threshold` gates endgame lock-in behavior

### Hard Bot

- **Exact Bayesian posterior** over 12 deck configurations, updated on every trade event
- Log-space hypergeometric likelihood to avoid numeric underflow at high card counts
- Tracks `pressure_[slot]` — per-player directional trade history used to infer goal suits
- **Posterior collapse detection** — when P(deck) > 0.90 for a single configuration, switches to deterministic lock-in mode
- **Card-count elimination** — rules out deck configs inconsistent with observed hand totals
- **Early market seeding** — if `early_seed_threshold` met, posts quotes before round pressure builds to obscure own accumulation

### Key Design Choice

Bot difficulty tiers differ in _information model quality_, not just parameter tuning. Easy uses no inference; Medium uses approximate Bayesian with noise; Hard uses exact Bayesian with opponent modeling. Same interface, qualitatively different strategies.

---

## Evaluation Framework

**Location:** `server/eval/`

An in-process plugin architecture running on a dedicated thread. `EvalRunner` maintains a lock-free SPSC queue fed by the game loop and fans events out to registered `EvalModule` implementations.

Three modules:

| Module | Output | Routing |
|---|---|---|
| `BayesianEvalModule` | Deck posteriors, goal-suit marginals, settlement EV, per-suit delta EV | Private per-slot |
| `AccumulationEvalModule` | Per-player behavioral signals (Normal / Elevated / High) with EWMA baseline | Broadcast |
| `ExecutionEvalModule` | Per-suit fill probability, aggressive vs. passive EV, spread cost, recommendation | Broadcast |

All eval data flows over the existing WebSocket connection as `eval.*` prefixed messages — no new endpoints.

---

## Auth, Security & API

**Location:** `server/include/server/`

- **OAuth 2.0** — GitHub and Google providers (`oauth_provider.h`)
- **JWT** — access (15 min) + refresh (7 day) tokens issued as HttpOnly cookies (`auth_service.h`)
- **API keys** — SHA-256 hashed, `ank_<64hex>` format, 30-day TTL, max 1 active key per player (`api_key_repo.h`, `crypto_util.h`)
- Bearer auth on WebSocket upgrade; `api_key_invalid` event + close on expiry or revocation
- **Spectator tokens** — short-lived single-use `stk_<hex>` tokens for browser spectator handoff without re-authentication (`spectate_token_repo.h`)
- All key generation uses `RAND_bytes` (OpenSSL); hashing via EVP SHA-256

---

## Frontend Architecture

**Location:** `frontend/`

React + TypeScript + Vite. State flows down from hooks; components are stateless consumers of props.

### Key Hooks

| Hook | Responsibility |
|---|---|
| `useWebSocket` | Single WS connection, full message type dispatch, all game state |
| `useKeyBinds` | Fetches per-player keybind overrides from REST; merges with defaults |
| `useKeyboardShortcuts` | Global `keydown` listener; dispatches 11 trading actions via inverted combo map |
| `useEvalMetrics` | Subscribes to `eval.*` messages; exposes posterior, accumulation, guidance state |
| `useGameConfig` | Reads `GameConfigContext`; exposes `gameMode`, `allowMultiQty`, `wipeOnTrade` flags |

### Pages & Components

```
/                  Landing page (public)
  ├─ ExpandingPortal        — GSAP scroll-pin hero, framer-motion layout spring, mouse parallax
  ├─ AutoAdvanceProgress    — scroll-driven accordion; step-by-step game rules; canvas chip anim
  ├─ ScrollTrackerSection   — 400vh sticky slideshow; SVG stepped-border frame; GSAP pill indicator
  └─ MathDive               — CSS 3D camera dive (EV → Bayes → order book → CTA); GSAP ScrollTrigger
/auth              OAuth buttons (Login)
/lobby             Lobby browser — list, create, join by code, active tab
/lobby/:code       Lobby room — roster, bot controls, start button
/game              Trading UI — 3-col layout in intermediate/advanced (left: feed+orders,
  │                             center: depth/mbo, right: overview+suits)
  ├─ MarketOverview + DeltaTable   — hand counts + per-player net card flow
  ├─ SuitPanel (×4)                — order form + best bid/ask; multi-qty inputs in
  │                                  intermediate/advanced modes
  ├─ MyOrders                      — resting orders + cancel; shows qty_remaining/qty
  ├─ TradeFeed                     — recent trade history
  ├─ MbpNDepthPanel                — full price-level depth ladder (intermediate center)
  ├─ MboFeedPanel                  — MBO event log: order_added/executed/cancelled (advanced center)
  ├─ InterRoundScreen              — standings + vote-to-end + countdown
  ├─ RoundEndModal                 — goal reveal, payouts
  ├─ GameEndScreen                 — final standings + per-round breakdown
  └─ EvalPanel                     — collapsible; posterior, accumulation signals, guidance
/spectate/:lobbyId  Read-only spectator view with script log panel
/settings/keybinds  Keybind customisation (GET/PUT /players/me/keybinds)
/api-keys           API key CRUD
/docs               Static API reference + script templates
/learn              Grouped eval-module documentation
```

`AppNav` — persistent top bar across all authenticated non-game routes; lives outside `AnimatePresence` so it never flashes during page transitions.

### Design Choices

- **CSS design tokens** — all colors, typography, radii, and spacing defined as CSS custom properties in `src/styles/tokens.css`; no magic numbers in component CSS
- **Route-level code splitting** — every page is `React.lazy()`; below-fold landing sections deferred behind a `Suspense` boundary; each route is its own chunk loaded on demand
- `BrowserRouter` with lobby UUID passed via navigation state; falls back to REST lookup for direct URL navigation
- `delta_update` is a full 4×4 snapshot replacement on every trade — no client-side accumulation
- All 11 keyboard shortcuts are configurable per player and persisted to the DB

---

## Wire Protocol

All messages are JSON with a `type` string discriminator. The single source of truth is `frontend/src/types/messages.ts`.

**Server → Client (selected):**

| Type | Description |
|---|---|
| `round_start` | Per-player: hand, slot, `round_end_at`, `game_mode` |
| `trade` | Broadcast; `your_side` personalized per recipient; carries `qty_filled`, `aggressor_order_id` |
| `book_update` | MBP-1: best bid/ask per suit; carries `seq` + `v` |
| `book_depth` / `book_depth_snapshot` | MBP-N: full price-level depth per suit (incremental / on connect) |
| `order_added` / `order_executed` / `order_cancelled` | MBO: individual order lifecycle events with `seq` + `v` |
| `order_book_snapshot` | MBO: full per-suit order list on connect or resync |
| `book_state_snapshot` | Bulk best-bid/ask reset for all suits at round start |
| `order_partially_filled` | Private to order owner: partial fill in advanced mode, updated `qty_remaining` |
| `delta_update` | Full 4×4 net card flow snapshot after every trade |
| `all_balances` | All slot balances after every trade |
| `hand_totals` | Total card count per slot after every trade |
| `inter_round` | Standings, vote tallies, countdown |
| `eval_posterior_update` | Private per-slot: deck posteriors, settlement EV |
| `eval_accumulation_signal` | Broadcast: per-player behavioral signals |
| `eval_execution_guidance` | Broadcast: per-suit execution recommendations |

**Client → Server (selected):**

| Type | Description |
|---|---|
| `submit_order` | `suit`, `side`, `price`, `qty` |
| `nudge` | Move best quote ±1 on a suit |
| `cancel_order` | By order ID; server scans all books |
| `resync` | Request a full snapshot for your current feed tier |
| `vote_to_end` | Majority vote ends game early |
| `add_bot` / `remove_bot` | Owner-only lobby bot management |
| `script_log` | Plain string (≤500 chars); forwarded to spectators |

---

## Database & Persistence

**Location:** `db/`

PostgreSQL with a versioned migration system — `DbMigrator` applies `db/migrations/` files in version order on every server start. RAII connection pool via libpqxx (`DbPool`).

Key tables: `players`, `lobbies`, `lobby_players`, `game_sessions`, `rounds`, `session_errors`, `player_keybinds`, `api_keys`, `spectate_tokens`.

No ORM — raw SQL via pqxx. Migrations are additive and forward-only.

---

## CLI & Scripted Players

**Location:** `cli/`

A Python CLI (`pip install -e cli/`) for terminal-based and scripted players using API-mode lobbies.

```bash
anjeer setup          # Save server URL + API key to ~/.anjeer/config.json
anjeer find           # List open API-mode lobbies
anjeer create         # Create a new API-mode lobby
anjeer join <code>    # Join and launch your trading script
```

The `join` command sets `ANJEER_API_KEY`, `ANJEER_SERVER_WS_URL`, and `ANJEER_LOBBY_CODE` env vars then `exec`s your configured script. Scripts communicate over WebSocket using the same wire protocol as the browser client. The spectator view in the browser shows a live `script_log` panel fed by messages from your script.

---

## Getting Started

### Prerequisites

| Tool | Version |
|---|---|
| g++ | 11+ |
| cmake | 3.20+ |
| Node.js | 18+ |
| npm | 9+ |
| PostgreSQL | 15 |
| libpqxx-dev | system package |
| libssl-dev | system package |
| clang-format | any recent |

On Ubuntu/Debian:
```bash
sudo apt install g++ cmake libpqxx-dev libssl-dev clang-format
```

### 1. Clone & install frontend dependencies

```bash
git clone git@github.com:devajya/anjeer.git
cd anjeer
npm install --prefix frontend
```

### 2. Create the database

```bash
createdb anjeer_dev
```

### 3. Configure

```bash
cp config/default.example.json config/default.json
```

Edit `config/default.json` and fill in:

- `db.connection_string` — e.g. `"postgresql:///anjeer_dev"`
- `auth.jwt_secret` — any long random string
- `auth.github.client_id` / `client_secret` — from a [GitHub OAuth App](https://github.com/settings/developers)
- `auth.google.client_id` / `client_secret` — from [Google Cloud Console](https://console.cloud.google.com/)

For local dev, set OAuth redirect URIs to `http://localhost:8080/auth/callback`.

### 4. Build & run

```bash
make dev
```

Builds the C++ server and frontend, then starts both concurrently. Open [http://localhost:5173](http://localhost:5173).

### 5. (Optional) Install the CLI

```bash
pip install -e cli/
anjeer setup   # prompts for server URL and API key
```

---

## Configuration

All tuneable values live in `config/default.json` — nothing is hardcoded. Local overrides go in `config/dev.json` (gitignored).

| Section | Controls |
|---|---|
| `server` | Host, WS port (9001), HTTP port (8080), heartbeat/ping intervals, CORS origin |
| `order_book` | Price range (1–99), initial nudge prices, active suits |
| `game` | Player count, total cards (40), countdown, round duration (240s), inter-round (30s) |
| `scoring` | Starting balance (400), pot size (200), points per card (10) |
| `db` | Connection string, pool size, migrations directory |
| `auth` | JWT secret, token TTLs, secure_cookies flag, OAuth client credentials |
| `lobby` | `min_players`, `max_players` |
| `bots` | Scheduler threads, tick intervals, jitter, network delay sim, per-difficulty strategy thresholds |
| `rate_limit` | Token bucket capacity, refill rate, suspension thresholds |

---

## Makefile Reference

```bash
make build          # Compile C++ (engine + server) + build frontend
make dev            # Build server, then run server + Vite in parallel (ports 9001 + 5173)
make test           # All C++ unit/integration tests + frontend Vitest
make test-unit      # C++ Catch2 tests only (via ctest)
make test-frontend  # Vitest only
make fmt            # clang-format all C++; prettier all TS/CSS
make clean          # Wipe build/ and frontend/dist (keeps .deps/ and node_modules/)
make clean-all      # Full reset including .deps/ and node_modules/
```

---

## Project Layout

```
anjeer/
├── engine/
│   ├── include/engine/    # Public headers (engine.h is the only façade)
│   ├── src/               # Implementation
│   └── tests/             # Catch2 unit tests
├── exchange/
│   ├── include/exchange/  # OrderBook, ExchangeSession, Sequencer, market_data types
│   ├── src/               # Implementation
│   └── tests/             # Catch2 tests (order book, sequencer, session)
├── server/
│   ├── include/server/    # Server headers
│   ├── src/               # WsServer, HttpServer, GameSession, repos
│   ├── eval/              # EvalRunner + analysis modules (Slice 11)
│   └── tests/             # Integration tests
├── frontend/
│   ├── src/
│   │   ├── components/    # React components
│   │   ├── pages/         # Route-level pages
│   │   ├── hooks/         # useWebSocket, useKeyBinds, useEvalMetrics, ...
│   │   └── types/         # messages.ts — wire protocol source of truth
│   └── vite.config.ts
├── cli/                   # Python CLI package
├── db/
│   └── migrations/        # Versioned SQL (applied automatically on server start)
├── config/
│   ├── default.example.json
│   └── default.json       # gitignored — copy from example
└── Makefile
```
