# Deferred Work, Known Issues & Architectural Notes

---

## Deferred Architectural Notes

### Engine directory split: `engine/bots/` → separate `bots/` and `exchanges/` modules

Consider splitting `engine/` into two top-level modules when switching to a structured wire protocol (Slice 17 / MessagePack):

- **`engine/exchanges/`** — `OrderBook`, `GameState`, `ScoringEngine`, `suit.h`, event types. Pure exchange mechanics.
- **`engine/bots/`** — `BotAgent`, `EasyBot`, `MediumBot`, `HardBot`, `BotConfig`, `BotGameSnapshot`, `BotAction`. Pure strategy.

Motivation: `BotAdapter`'s snapshot-building logic will grow into a non-trivial deserialization layer as the wire protocol matures. The natural forcing function is the Slice 17 serialization overhaul — one pass through the wire format rather than two.

---

## Deferred Validation Work (Layer 3 / Layer 4 — not in CI)

Belongs in offline research tooling gated behind a CMake option or separate harness.

- **Monte Carlo win-rate validation** — N > 10,000 simulated rounds; verify High accumulation signals correlate with wins above chance. Requires Slice 10 + Slice 12 replay infrastructure.
- **Confidence-to-win-rate calibration curve** — plot P(win | confidence = x), verify monotonicity and Brier score. Requires empirical replay logs.
- **ROC / AUC evaluation** — treat signal levels as binary classifiers for "player holds the most goal cards"; compute AUC over a ground-truth test set.
- **Simulated agent tournaments** — scripted agents with known accumulation patterns; measure eval module identification accuracy. Appropriate for Slice 10 headless harness.
- **Latency-vs-accuracy tradeoff experiments** — sweep EWMA alpha and `on_round_end` rate limits; measure signal quality degradation. Offline profiling only.
- **Human usefulness studies** — A/B tests on whether displaying eval signals changes player decision quality or win rates. Out of scope for automated testing.
- **Execution and Bayesian module calibration** — fill-probability accuracy and posterior sharpness studies; requires Slice 12 game recording replay logs.

---

## Known Issues & Deferred Fixes

### Architecture

- **Instrument ID scheme** — current approach uses array indexes, which gets tricky with cancel routing. `handle_cancel` does a linear probe across all suits (O(n_suits)) because the wire protocol omits suit from cancel messages. If `ExchangeSession` maintained an `order_id → instrument_id` index, cancel dispatch becomes O(1).
  - `LOCATION:` `server/src/game_session.cpp:364-396`
  - `SEVERITY:` low

- **ExecutionEvalModule scope** — correctly estimates fill probability, spread capture, and leakage risk, but has no inventory or belief state. The `action` field means "passive quoting favorable / crossing favorable / no action" — not buy/sell/hold. True directional recommendations require per-slot broadcasts, Bayesian posterior input, and inventory conditioning. Do not bolt sell logic onto this module; build a separate directional module when the time comes.

### Gameplay / Lobby

- **Spectate latency on first API-mode game** — non-creator spectators lag on the first game after `make dev`; subsequent games are fine. Suspected: WS handle registration timing relative to game-loop thread start.
- **Script log leakage to all spectators** — `script_log` messages from any player in an API-mode game are forwarded to all spectators. Fix: link `spectator_id` back to originating player slot.
- **Lobbies not live-updated** — lobby list and lobby room are REST-only with no WebSocket or polling. Three sub-issues:
  - Players in queue are not notified or ejected when a game ends; queue state silently becomes stale.
  - Weird redirect flow when: join game → leave → start creating new game → change mind → hit back.
  - No live updates for player join/leave, bot changes, or lobby status without manual refresh.
- **5-bot lobby silently allows a 6th player** — creating a lobby with 5 bots then adding a player does not surface an error; the bot is killed silently without notifying the lobby creator.
- **Duplicate bot names change each other's difficulty** — in the bot lobby UI, if two bots share a name, changing one's difficulty changes both. Fix: disallow same-name bot spawn in one lobby.

### Session / Reconnect

- **Reconnect token only reliable after second round** — token issued before game-loop thread has fully initialized slot state for round 1.
- **Reconnect / session-expired modal delayed ~3 seconds** — inter-round countdown timer appears to hold rendering priority over the modal.
- **Server crash on next-round start after close-tab → re-enqueue reconnect** — sequence: disconnect → reconnect window → queue admit → round ends → `owner_start_round` → crash. Suspected double-claim on reconnect timer and queue-admission path.

### Bots

- **Bots share a log file when same difficulty in same lobby** — log path is `bot-<difficulty>.log` without slot index. Fix: append `-slot<N>`.
- **`spawn_on_player_leave` naming inconsistency** — JSON key and `ServerConfig` use `spawn_on_player_leave`; `Lobby` struct and `GameSession` use `spawn_bots_on_leave`. Rename JSON key and `ServerConfig` field to match.
- **Bot tick default mismatch** — `ServerConfig::BotsConfig::tick_interval_ms` defaults to 1500 ms in code but `config/default.example.json` says 500 ms. Align the in-code default.
- **Bot replacement name not reflected in MarketOverview** — old bot name persists after `game_bot_replaced` until the next re-render.

---

## Slice Order Rationale

Slices 1–4 build and validate the core engine in isolation before auth or persistence complexity is introduced. Slice 5 wires identity. Slice 6 introduces the lobby system and `IEventBus` abstraction — the foundation for multi-node production without local dev overhead. Slice 6.5 adds observability immediately after the lobby so `GameSession` crashes surface as typed errors rather than silent hangs. Slice 7 completes the game lifecycle and threading model; it folds in lobby-layer resilience fixes as small prerequisites. Slice 7.5 adds in-game connection resilience after `GameSession` exists. Slices 8–9 make the game usable by both player types. Slices 10–11 add depth. Slice 11.5 introduces the tiered market data feed after bots (Slice 10) provide multi-session load to validate the endpoint and after eval (Slice 11) establishes separate event namespaces; it precedes recording (Slice 12) so the recorder captures raw order events from the start. Slices 12–14 form an atomic review pipeline. Slice 15 is polish. Slice 16 is the first time multiple nodes exist and Redis is required — `RedisEventBus` (stubbed since Slice 6) is completed here. Slice 17 generalizes the engine for richer game configurations and folds in MessagePack binary encoding since the serialization layer is already being overhauled.
