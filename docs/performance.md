# Performance

Measured 2026-08-14 on a local development machine (i7-12700H, 20 threads, 16 GB).
Reproduce with `make metrics`. Raw output lands in `docs/metrics-<date>.md`.

Headline: the matching engine runs about six orders of magnitude faster than the
game demands, and user-visible latency is set almost entirely by the 16 ms
outbound drain rather than by any computation. Server capacity is bounded by CPU,
not memory, and scales linearly with the number of active lobbies.

## Measured

| Metric | Value | Conditions |
|---|---|---|
| OrderBook throughput | 13.4M orders/sec, 74 ns/order | `game_scale`: 4 books, 5 players, wipe on trade |
| Order to ack | p50 20.5 ms, p95 21.3 ms, p99 21.6 ms | n=240, 100% acked, single lobby |
| Concurrent capacity | 24 lobbies / 96 players with no degradation | p99 stayed flat across the whole ramp |
| CPU per lobby | 1.75% of one core, linear from 3 to 16 lobbies | 4 players per lobby at 4 orders/sec each |
| Memory per lobby | 0.5 MB, over a 15 to 31 MB baseline | |
| Frontend render | 3.4 ms per market-data burst | 1008 profiled commits, actual/base 0.846 |

Lighthouse is not included. See "Known gaps" below.

## What the numbers mean

The latency distribution is quantised, not loaded. p50 of 20.5 ms sits above the
mean of 16.9 ms, and p99 is only 1.1 ms above p50. That shape comes from the
16 ms outbound drain in `WsServer`: an ack waits for the next tick regardless of
how quickly the exchange matched it. The consequence is that p99 did not move at
all between 1 and 24 concurrent lobbies. Latency here is a scheduling choice, not
a capacity signal, and halving it is a matter of halving the drain interval and
paying for it in bandwidth.

The `game_scale` figure of 13.4M orders/sec is the one to quote rather than the
18.8M from `match_heavy`. `match_heavy` crosses every second order, which is not
what a real game does. Against a real lobby submitting roughly 16 orders/sec, even
the conservative number is about five orders of magnitude of headroom, so the
matching engine is not a bottleneck at any plausible player count.

CPU scales cleanly with lobby count: 5.5% of one core at 3 lobbies, 17% at 8, 28%
at 16. That linearity is expected given the threading model, since each lobby owns
a `GameSession` thread and an `EvalRunner` worker and shares nothing mutable with
the others.

## Extrapolation to the production t3.micro

These are estimates, not measurements. Nothing in this section has been run
against EC2.

A t3.micro has 2 burstable vCPUs and 1 GiB of memory. It runs at a 10% baseline
and earns 6 CPU credits per hour, capped at 144, where one credit buys one vCPU
running flat out for one minute. A t3 vCPU is a hyperthread on a Xeon in the 2.5
to 3.5 GHz range, so it should be somewhere between two and two and a half times
slower than one core of the i7 these numbers came from. That puts a lobby at
roughly 4% of one t3 vCPU, against a sustained baseline budget of 20% of one vCPU.

| Scenario | Estimate |
|---|---|
| Sustained at baseline, credits exhausted | about 5 lobbies, 20 players |
| Bursting with a full credit balance | about 25 lobbies for 2.5 hours, or 50 for 75 minutes |
| Memory ceiling | roughly 1,400 lobbies, so not a factor |
| p99 latency, server side | unchanged at 21 to 22 ms until CPU saturates |

Two adjustments push in opposite directions. TLS termination runs in nginx on the
same instance and was not part of the local test, which used plaintext, so add
somewhere around 15 to 30% CPU. Against that, the synthetic load is far heavier
than real play: the harness drives 4 orders per second per player where a human
manages perhaps 0.1 to 0.5. Some of the 1.75% is fixed per-session cost that does
not shrink with order rate, since the 16 ms tick and the eval thread run whether
or not anyone trades, so the saving is smaller than the ratio suggests. Taking
both together, sustained capacity at realistic play is plausibly 2 to 5 times the
table above, in the range of 10 to 25 lobbies at baseline.

The binding constraint is CPU credits. Memory is irrelevant at this scale.

## Known gaps

The fixed versus variable split in per-lobby CPU has not been separated. One ramp
at a realistic order rate would settle it and would tighten the capacity estimate
considerably.

Nothing has been measured on EC2. The cheap version is not a full ramp: five
lobbies for five minutes, with the client running on the instance rather than over
the internet, would confirm the scaling factor and the TLS overhead for about five
credits. Measuring order-to-ack from a laptop would mostly measure the laptop's
internet connection, since round trip time would dominate the 21 ms server number.

Lighthouse has never run. `make lighthouse` invokes `npx lhci autorun`, but
`@lhci/cli` is not a declared dependency, so npx resolves the bare name `lhci`
from the public registry, which is an unrelated placeholder package. It executes,
prints a greeting, and produces no report, which is why `.lighthouseci/` is empty.
The target is not wired into CI, so this has only ever run locally. Fixing it
means adding `@lhci/cli` to the frontend dev dependencies.

## Defect found while measuring

The msgpack subprotocol is negotiated but never applied. A client that connects
with `Sec-WebSocket-Protocol: anjeer-msgpack` gets the subprotocol accepted and
then receives zero binary frames. Across 240 market-data frames the payloads were
byte-identical to the JSON client's, at 122 bytes each.

The send path in `ws_server.cpp` checks `slots_[slot].encoding`, and
`GameSession` sets that field from the connect event. But only bot adapters
enqueue `NetConnect` (`ws_server.cpp:857`); for real clients the slot encoding is
only ever populated on the reconnect path (`ws_server.cpp:1658`). A fresh
connection therefore keeps the `Encoding::JSON` default for the lifetime of the
session. Browser clients are affected as well as API clients, so the feature is
currently inert in production.

Market data is about 8% of the bytes a client receives, so fixing this is worth
roughly a 3 to 4% reduction in total downstream traffic, assuming msgpack saves
40 to 50% on those frames.

## Harness

`scripts/collect_metrics.py`, run via `make metrics`. It records and never fails,
because the point is capturing what the number currently is.

The live metrics need a running server and `psql`. API keys cannot be minted over
REST, since `POST /players/me/api-keys` requires a JWT cookie from an OAuth login
and the schema allows one active key per player, so the harness seeds players and
key hashes directly and tags every row `oauth_provider='bench'` for cleanup.

Three details are load-bearing and were each found by getting them wrong first.
Clients must connect after `POST /lobbies/<id>/start`, not before, or they attach
as plain lobby sockets and the session ends itself with "all disconnected for
>500ms". That start endpoint resolves by UUID only, while `/join` accepts a code
or a UUID. Submissions must stay under `rate_limit.refill_rate`, which is 5 per
second per connection, or the token bucket rejects most of them and the ack rate
collapses. The harness derives its pacing from config rather than hardcoding it.

The msgpack comparison uses two clients that never trade. A trading client also
receives `order_ack` and other personalised frames, so comparing a trading msgpack
client against a trading JSON client compares two different message mixes rather
than two encodings of the same stream.
