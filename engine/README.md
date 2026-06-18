# Engine & Exchange

The game's core is split into two pure C++ static libraries — `engine/` and `exchange/` — with no network, filesystem, or JSON dependencies. This separation means both can be tested in complete isolation and neither ever blocks on I/O.

## Exchange Layer

The exchange sits between the game server and the engine. It owns everything related to market mechanics: order matching, sequence numbering, and the event model.

**Order matching** is price-time priority. All prices are integers — no floating point anywhere in the matching path. The exchange supports three operation modes driven by game config: simple mode (full book wipe after every trade), intermediate (multi-quantity orders, MBP-N depth), and advanced (partial fills, no wipe, MBO event log).

**Dual-return event API** — every exchange operation returns two streams: `feedback` (private response to the submitting player: ack, reject, fill) and `market` (observable events broadcast to all participants: order added, executed, cancelled). This split keeps the server's routing logic clean — it never has to decide mid-function who should see what.

**Sequencer** — a monotonic sequence counter shared across all instruments in a session. Sequence numbers reset at round start so clients can detect gaps and request resyncs without tracking absolute counters.

## Engine Layer

Pure game logic — no market mechanics.

**Game state** tracks per-player hand counts and manages deck dealing. There are 12 pre-defined deck configurations, each specifying per-suit card distribution and an explicit goal suit. The goal suit is set directly in the deck definition rather than derived from counts — this prevents ambiguity when distributions are symmetric.

**Scoring** is a pure function with no side effects. It takes hands, goal suit, disconnected flags, and scoring config, then returns payouts. The server derives the bonus pool and passes it in at scoring time rather than baking the calculation into the engine — keeping the engine's inputs explicit and the arithmetic auditable.

## Bot Strategies

Three difficulty tiers all implement the same `BotAgent` interface. The tiers differ in **information model quality**, not just speed or aggression.

**Easy** — hand-heuristic only. No inference about what other players hold.

**Medium** — opens with a multivariate hypergeometric posterior over the 12 deck configurations based on its starting hand, then updates noisily on observed trades. Makes EV-threshold decisions for maker and taker orders.

**Hard** — exact Bayesian posterior updated on every trade event. Additionally tracks per-player directional trade pressure to infer goal suits, detects when its posterior has collapsed to near-certainty and locks in, eliminates impossible deck configurations using card-count arithmetic, and uses depth reading to avoid thin quotes that may be bait. Bid prices are discovered dynamically using an EV-bounded strike mechanism rather than fixed levels.

The `BotAdapter` bridges each strategy to the server's threading model — it runs on the `BotScheduler` thread pool and communicates with the game session exclusively through lock-free queues, never touching shared state directly.
