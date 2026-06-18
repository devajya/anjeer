# Server & Backend

The server is written in C++ and handles WebSocket connections, HTTP/REST, authentication, game session management, bot coordination, and real-time analysis. It exposes two ports: port 9001 (uWebSockets, game traffic) and port 10000 (Crow, REST and health).

## Threading Model

The server runs four independent threading axes. All cross-thread data transfer uses lock-free SPSC queues — no mutexes on the hot path.

- **uWS event loop** — parses inbound WebSocket messages and dispatches them to the appropriate session queue; drains outbound game events every 16ms and sends them to clients
- **GameSession thread** (one per active lobby) — owns all game logic, round timers, scoring, and DB writes; communicates with the network thread exclusively via two SPSC queues
- **BotScheduler thread pool** — ticks each bot adapter on a configurable schedule with jitter to produce human-like timing; bot actions are enqueued back into the session's inbound queue
- **EvalRunner worker thread** (one per active session) — receives game events from the session via a 64-slot SPSC queue and fans them to the analysis modules; drops silently at capacity to avoid back-pressure on the game loop

Keeping game logic on a dedicated thread per session means a slow session never stalls another. The SPSC queue boundary also makes the threading model easy to reason about — there is no shared mutable state between axes.

## Game Session

`GameSession` runs a `SessionPhase` state machine: `Lobby → Countdown → RoundActive → InterRound → Ended`. It manages the full game lifecycle: deck selection, dealing, buy-ins, round timers, post-trade pipelines, scoring, and inter-round votes. Session and round records are persisted to PostgreSQL after each phase transition.

## HTTP Server & Auth

`HttpServer` handles OAuth 2.0 (GitHub and Google), JWT issuance, and all REST endpoints. Tokens are issued as HttpOnly cookies — the frontend never touches them directly, which removes a class of XSS risk. Access tokens expire in 15 minutes; refresh tokens in 7 days.

API key auth is available for scripted players: keys are SHA-256 hashed at rest, presented as `ank_<64hex>`, and validated on WebSocket upgrade. A revoked or expired key closes the connection immediately.

Spectator tokens (`stk_<hex>`) allow a browser session to be handed off to a spectator without re-authenticating through OAuth — they are single-use and short-lived.

## Eval Framework

Three analysis modules run on a dedicated thread per active session:

| Module | What it produces | Who sees it |
|---|---|---|
| `BayesianEvalModule` | Deck posteriors, goal-suit probabilities, settlement EV | Private per player |
| `AccumulationEvalModule` | Behavioral signals per player (Normal / Elevated / High) using EWMA | Broadcast |
| `ExecutionEvalModule` | Fill probability, passive vs aggressive EV, spread cost | Broadcast |

Running eval on its own thread keeps it from adding latency to the game loop. The SPSC queue between the game thread and the eval worker is bounded — if the eval worker falls behind, events are dropped rather than blocking the game.

## Production

The server binary runs as a systemd service with `Restart=always`. Secrets (DB connection string, JWT secret, OAuth credentials) are injected at runtime via `EnvironmentFile` — `config/prod.json` is committed with placeholder sentinel strings and never contains real values.

On startup, the server runs any unapplied DB migrations, prunes expired spectate tokens and closed lobbies, then opens its ports. `GET /health` executes a lightweight `SELECT 1` against the connection pool and is used by the deploy workflow smoke test.

CI runs on every push (build + ctest + Vitest + gitleaks secret scan). CD triggers on push to `main`: builds the binary and frontend, ships a tarball to EC2 over SCP, runs migrations, swaps the binary, restarts the service, and confirms `/health` returns 200.
