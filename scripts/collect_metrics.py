#!/usr/bin/env python3
"""
Metrics harness — collects every performance number the project can currently
prove, in one run, and writes a dated report.

Run with `make metrics` (see Makefile). Nothing here asserts or fails: it is a
recorder, not a gate. A number that regressed is still a number worth having.

WHAT IT MEASURES
----------------
  orderbook   OrderBook throughput, ops/sec           offline  (make bench-exchange)
  render      React commit time per market burst      offline  (make test-frontend-perf)
  lighthouse  Landing page performance score          offline  (make lighthouse)
  latency     order -> order_ack, p50/p95/p99         LIVE
  capacity    max concurrent lobbies before slip      LIVE
  rss         server RSS per active lobby             LIVE + local (or --ssh)
  wire        msgpack vs JSON broadcast bytes         LIVE

The LIVE metrics need a running server and seeded API keys. Keys cannot be
minted over REST -- POST /players/me/api-keys requires a JWT cookie from an
OAuth login -- so this script inserts players and key hashes directly, matching
ApiKeyRepo::create (sha256_hex of the plaintext, one active key per player).

DEPENDENCIES
------------
  pip install websockets      (already a cli/ dependency)
  psql on PATH                (seeding + cleanup)
"""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import os
import re
import secrets
import shutil
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass, field
from datetime import date
from pathlib import Path

# Imported lazily: the offline metrics (and --help) must work without it.
websockets = None


def require_websockets():
    global websockets
    if websockets is None:
        try:
            import websockets as _ws
        except ImportError:
            return False
        websockets = _ws
    return True


REPO = Path(__file__).resolve().parent.parent

# Lobbies need lobby.min_players to start; every seeded client is a real player.
PLAYERS_PER_LOBBY = 4
SUITS = ["clubs", "diamonds", "hearts", "spades"]

# The frame types server/src/market_data_wire.cpp has msgpack encoders for.
# Everything else is JSON regardless of the negotiated subprotocol.
MARKET_DATA_TYPES = {
    "book_update", "book_depth", "book_depth_snapshot",
    "order_added", "order_executed", "order_cancelled", "book_snapshot",
}

# A lobby is "healthy" while acks stay under this. Past it the game thread is
# not keeping up with its 16ms outbound drain and the ramp stops.
ACK_P99_BUDGET_MS = 50.0


# ── Result plumbing ───────────────────────────────────────────────────────────

@dataclass
class Metric:
    key: str
    label: str
    value: str = "—"
    detail: str = ""
    ok: bool = False


@dataclass
class Report:
    target: str
    metrics: list[Metric] = field(default_factory=list)

    def add(self, key, label, value="—", detail="", ok=False):
        self.metrics.append(Metric(key, label, str(value), detail, ok))

    def get(self, key):
        return next((m for m in self.metrics if m.key == key), None)


def node_env():
    """Vitest and Lighthouse need node >= 18, but a shell's default `node` is
    often an older nvm version. Find the newest installed toolchain and put it
    at the front of PATH for the frontend subprocesses only."""
    if shutil.which("node"):
        code, out = run(["node", "--version"], timeout=15)
        if code == 0 and out.strip().startswith("v"):
            try:
                if int(out.strip()[1:].split(".")[0]) >= 18:
                    return {}
            except ValueError:
                pass
    versions = Path.home() / ".nvm" / "versions" / "node"
    if not versions.is_dir():
        return {}

    def key(p):
        try:
            return tuple(int(x) for x in p.name.lstrip("v").split("."))
        except ValueError:
            return (0,)

    usable = sorted((p for p in versions.iterdir() if (p / "bin").is_dir()), key=key)
    if not usable:
        return {}
    return {"PATH": f"{usable[-1] / 'bin'}:{os.environ.get('PATH', '')}"}


def log(msg):
    print(f"  {msg}", flush=True)


def section(name):
    print(f"\n\033[1m{name}\033[0m", flush=True)


def run(cmd, cwd=REPO, timeout=900, env=None):
    """Run a command, capture combined output. Never raises on non-zero."""
    merged = {**os.environ, **(env or {})}
    try:
        p = subprocess.run(cmd, cwd=cwd, shell=isinstance(cmd, str),
                           capture_output=True, text=True, timeout=timeout,
                           env=merged)
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except subprocess.TimeoutExpired:
        return 124, "TIMEOUT"
    except FileNotFoundError as e:
        return 127, str(e)


# ── Config ────────────────────────────────────────────────────────────────────

def load_config(target):
    """Read connection details from config/. Prod falls back to the example
    file for structure but expects real values in prod.json."""
    name = "prod.json" if target == "prod" else "default.json"
    path = REPO / "config" / name
    if not path.exists():
        path = REPO / "config" / "default.example.json"
        log(f"config/{name} missing, falling back to {path.name}")
    cfg = json.loads(path.read_text())
    return cfg


def endpoints(cfg, target, http_override, ws_override):
    if target == "prod":
        http = http_override or "https://anjeer.duckdns.org"
        ws = ws_override or "wss://anjeer.duckdns.org/ws"
    else:
        http = http_override or f"http://localhost:{cfg['server']['http_port']}"
        ws = ws_override or f"ws://localhost:{cfg['server']['port']}/ws"
    return http, ws


# ── Seeding (direct DB, because REST cannot mint keys headlessly) ─────────────

class Seeder:
    """Creates throwaway players + API keys, and removes them afterwards.

    Every row it writes is tagged oauth_provider='bench' so cleanup can never
    touch a real account.
    """

    TAG = "bench"

    def __init__(self, conn_string):
        self.conn = conn_string
        self.keys: list[str] = []
        self.usernames: list[str] = []

    def _psql(self, sql):
        code, out = run(["psql", self.conn, "-v", "ON_ERROR_STOP=1",
                         "-tA", "-c", sql], timeout=60)
        if code != 0:
            raise RuntimeError(f"psql failed: {out.strip()[:400]}")
        return out.strip()

    def available(self):
        return shutil.which("psql") is not None

    def seed(self, count):
        """Returns `count` plaintext API keys usable as Bearer tokens."""
        run_id = secrets.token_hex(4)
        values, keys = [], []
        for i in range(count):
            plaintext = "ank_" + secrets.token_hex(32)          # ank_<64hex>
            key_hash = hashlib.sha256(plaintext.encode()).hexdigest()
            username = f"bench_{run_id}_{i}"
            keys.append(plaintext)
            self.usernames.append(username)
            values.append((username, f"{run_id}-{i}", key_hash))

        rows = ",".join(
            f"('{u}','{self.TAG}','{oid}','{h}')" for u, oid, h in values
        )
        self._psql(
            "WITH ins AS ("
            f"  INSERT INTO players (username, oauth_provider, oauth_id) "
            f"  SELECT v.u, v.p, v.o FROM (VALUES {rows}) "
            f"    AS v(u,p,o,h) RETURNING id, username"
            ") "
            "INSERT INTO api_keys (player_id, key_hash, name, expires_at) "
            "SELECT ins.id, v.h, 'bench', NOW() + INTERVAL '1 day' "
            f"FROM ins JOIN (VALUES {rows}) AS v(u,p,o,h) ON v.u = ins.username;"
        )
        self.keys = keys
        return keys

    def cleanup(self):
        """Deletes only rows this run created, in FK order.

        api_keys / game_slots / keybinds / reconnect_tokens cascade off players,
        and rounds cascade off game_sessions. But lobbies.creator_id,
        lobby_players.player_id, game_sessions.lobby_id and
        spectate_tokens.player_id are all NO ACTION, so those four have to go
        first and in this order or the player delete is refused.
        """
        if not self.usernames:
            return
        names = ",".join(f"'{u}'" for u in self.usernames)
        players = f"(SELECT id FROM players WHERE username IN ({names}))"
        lobbies = f"(SELECT id FROM lobbies WHERE creator_id IN {players})"
        try:
            self._psql(
                f"DELETE FROM game_sessions WHERE lobby_id IN {lobbies};"
                f"DELETE FROM lobby_players WHERE player_id IN {players};"
                f"DELETE FROM lobby_players WHERE lobby_id IN {lobbies};"
                f"DELETE FROM spectate_tokens WHERE player_id IN {players};"
                f"DELETE FROM lobbies WHERE creator_id IN {players};"
                f"DELETE FROM players WHERE oauth_provider = '{self.TAG}' "
                f"  AND username IN ({names});"
            )
        except RuntimeError as e:
            log(f"cleanup warning: {e}")


# ── REST helpers ──────────────────────────────────────────────────────────────

def http_json(method, url, key=None, body=None, timeout=15):
    """urllib rather than requests: keeps this script dependency-light."""
    import urllib.error
    import urllib.request

    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Content-Type", "application/json")
    if key:
        req.add_header("Authorization", f"Bearer {key}")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = r.read().decode()
            return r.status, (json.loads(raw) if raw else {})
    except urllib.error.HTTPError as e:
        raw = e.read().decode()
        try:
            return e.code, json.loads(raw)
        except json.JSONDecodeError:
            return e.code, {"raw": raw}
    except Exception as e:                                   # noqa: BLE001
        return 0, {"error": str(e)}


# ── The live client ───────────────────────────────────────────────────────────

class BenchClient:
    """One scripted player. Submits orders and times the ack that comes back.

    Ack matching: the server does not echo a client-supplied id, so orders are
    sent one at a time per client and the next order_ack is attributed to the
    outstanding one. That keeps the measurement honest at the cost of pipelining
    -- which is what we want, since we are measuring round-trip, not throughput.
    """

    def __init__(self, key, ws_url, encoding="json"):
        self.key = key
        self.ws_url = ws_url
        self.encoding = encoding
        self.ws = None
        self.slot = None
        self.in_round = False
        self.latencies_ms: list[float] = []
        self.bytes_in = 0
        self.frames_in = 0
        # Only market-data frames are msgpack-encoded; everything else stays
        # JSON on both sockets. Account those separately or the comparison is
        # diluted by identical frames and reports a ~0% difference.
        self.md_bytes = 0
        self.md_frames = 0
        self.binary_frames = 0
        self.errors = 0
        self._ack = None

    async def connect(self):
        subproto = ["anjeer-msgpack"] if self.encoding == "msgpack" else None
        self.ws = await websockets.connect(
            self.ws_url,
            additional_headers={"Authorization": f"Bearer {self.key}"},
            subprotocols=subproto,
            max_size=4 * 1024 * 1024,
            open_timeout=15,
        )

    async def pump(self, stop: asyncio.Event):
        """Reads frames until stopped, accounting bytes and resolving acks."""
        try:
            while not stop.is_set():
                try:
                    raw = await asyncio.wait_for(self.ws.recv(), timeout=1.0)
                except asyncio.TimeoutError:
                    continue
                self.frames_in += 1
                binary = isinstance(raw, (bytes, bytearray))
                size = len(raw) if binary else len(raw.encode())
                self.bytes_in += size

                # A binary frame is by definition a msgpack market-data frame:
                # the server only reaches for msgpack on that path.
                if binary:
                    self.binary_frames += 1
                    self.md_bytes += size
                    self.md_frames += 1
                    continue
                try:
                    msg = json.loads(raw)
                except json.JSONDecodeError:
                    continue

                t = msg.get("type")
                if t in MARKET_DATA_TYPES:
                    self.md_bytes += size
                    self.md_frames += 1
                if t == "round_start":
                    self.slot = msg.get("player_slot")
                    self.in_round = True
                elif t in ("round_end", "game_ended"):
                    self.in_round = False
                elif t == "order_ack" and self._ack and not self._ack.done():
                    self._ack.set_result(time.perf_counter())
                elif t == "error":
                    self.errors += 1
                    if self._ack and not self._ack.done():
                        self._ack.set_exception(RuntimeError(msg.get("message", "err")))
        except websockets.ConnectionClosed:
            self.in_round = False

    async def timed_order(self, suit, side, price, timeout=5.0):
        """Sends one order, returns round-trip ms, or None if it never acked."""
        self._ack = asyncio.get_running_loop().create_future()
        t0 = time.perf_counter()
        try:
            await self.ws.send(json.dumps({
                "type": "submit_order", "suit": suit,
                "side": side, "price": price, "qty": 1,
            }))
            t1 = await asyncio.wait_for(self._ack, timeout=timeout)
        except (asyncio.TimeoutError, RuntimeError, websockets.ConnectionClosed):
            return None
        finally:
            self._ack = None
        ms = (t1 - t0) * 1000.0
        self.latencies_ms.append(ms)
        return ms

    async def close(self):
        if self.ws:
            try:
                await self.ws.close()
            except Exception:                                # noqa: BLE001
                pass


async def open_lobby(keys, http, ws_url, encodings=None):
    """Creates one API-mode lobby, joins every client, starts the game.

    Returns (clients, stop_event, pump_tasks, lobby_code) or None on failure.
    """
    owner, rest = keys[0], keys[1:]
    status, lobby = http_json("POST", f"{http}/lobbies", owner,
                              {"mode": "api", "game_mode": "simple"})
    if status != 201:
        log(f"lobby create failed: {status} {lobby}")
        return None
    code = lobby.get("code")
    # /join takes a code or a uuid, but /start resolves by uuid only and 500s on
    # a code -- so hold on to both and use the right one at each call.
    lobby_id = lobby.get("id")

    for k in rest:
        st, body = http_json("POST", f"{http}/lobbies/{code}/join", k)
        if st not in (200, 201):
            log(f"join failed: {st} {body}")
            return None

    # Order matters: the socket only attaches to a game slot if a session
    # already exists for the lobby. Connecting before /start yields a plain
    # "lobby socket", the session sees zero attached players, and it kills
    # itself with "all disconnected for >500ms". So start first, then connect
    # -- which is exactly what the CLI does before launching a bot script.
    st, body = http_json("POST", f"{http}/lobbies/{lobby_id}/start", owner)
    if st not in (200, 201, 204):
        log(f"start failed: {st} {body}")
        return None

    encodings = encodings or ["json"] * len(keys)
    clients = [BenchClient(k, ws_url, e) for k, e in zip(keys, encodings)]
    # Concurrently: the countdown is ~3s and the session drops if nobody is
    # attached shortly after the round opens.
    try:
        await asyncio.gather(*(c.connect() for c in clients))
    except Exception as e:                                   # noqa: BLE001
        log(f"ws connect failed: {e}")
        return None

    stop = asyncio.Event()
    tasks = [asyncio.create_task(c.pump(stop)) for c in clients]

    # Countdown is 3s by default; wait for the first round to actually open.
    for _ in range(60):
        if any(c.in_round for c in clients):
            break
        await asyncio.sleep(0.5)
    else:
        log("round never started")
        stop.set()
        return None

    return clients, stop, tasks, code


async def teardown(bundle):
    if not bundle:
        return
    clients, stop, tasks, _ = bundle
    stop.set()
    for c in clients:
        await c.close()
    for t in tasks:
        t.cancel()
    await asyncio.gather(*tasks, return_exceptions=True)


# ── Metric: order book throughput (offline) ───────────────────────────────────

def metric_orderbook(rep, perf_n):
    section("OrderBook throughput")
    code, out = run(["make", "bench-exchange"], env={"PERF_N": str(perf_n)})
    if code != 0:
        rep.add("orderbook", "OrderBook throughput", detail=f"make failed: {out[-200:]}")
        log("FAILED")
        return

    # Rows look like: "game_scale   72.2   13856131   4 books, 5 players, ..."
    rows = {}
    for line in out.splitlines():
        m = re.match(r"\s*([a-z_][a-z0-9_]*)\s+([\d.]+)\s+(\d+)\s", line)
        if m:
            rows[m.group(1)] = (float(m.group(2)), int(m.group(3)))
    if not rows:
        rep.add("orderbook", "OrderBook throughput", detail=out[-600:])
        log("could not parse; raw output kept in report")
        return

    # game_scale is the headline: it is the only scenario shaped like a real
    # game (4 books, 5 players, wipe-on-trade). match_heavy is faster but it is
    # a best case, and quoting a best case is how a number stops being believed.
    head = "game_scale" if "game_scale" in rows else max(rows, key=lambda k: rows[k][1])
    ns, ops = rows[head]
    human = f"{ops/1e6:.1f}M orders/sec" if ops >= 1e6 else f"{ops:,} orders/sec"
    others = ", ".join(f"{k} {v[1]/1e6:.1f}M" for k, v in rows.items() if k != head)
    rep.add("orderbook", "OrderBook throughput", human,
            f"{head}: {ns:.1f} ns/order. Also: {others}", ok=True)
    log(f"{human}  ({head}, {ns:.1f} ns/order)")


# ── Metric: React render cost (offline) ───────────────────────────────────────

def metric_render(rep):
    section("Frontend render cost")
    code, out = run(["make", "test-frontend-perf"], env=node_env())
    m = re.search(r"PERF_RESULT\s+(\{.*\})", out)
    if not m:
        rep.add("render", "Render per market burst",
                detail=f"no PERF_RESULT (exit {code})")
        log("FAILED")
        return
    r = json.loads(m.group(1))
    value = f"{r.get('avgActualMs', 0):.3f} ms/update"
    detail = (f"{r.get('commits')} commits, "
              f"max {r.get('maxActualMs')} ms, "
              f"actual/base {r.get('memoRatioActualOverBase')}")
    rep.add("render", "Render per market burst", value, detail, ok=True)
    log(f"{value}  ({detail})")


# ── Metric: Lighthouse (offline) ──────────────────────────────────────────────

def metric_lighthouse(rep):
    section("Lighthouse")

    # `make lighthouse` runs `npx lhci autorun`, but the real Lighthouse CI is
    # published as @lhci/cli -- the bare name `lhci` is an unrelated placeholder
    # package on the public registry. If @lhci/cli is not installed locally, npx
    # will happily download and execute that stranger's package instead, so
    # refuse to run rather than report whatever it prints as a score.
    pkg = REPO / "frontend" / "node_modules" / "@lhci" / "cli"
    if not pkg.exists() and not (REPO / "node_modules" / "@lhci" / "cli").exists():
        rep.add("lighthouse", "Lighthouse performance",
                detail="@lhci/cli not installed — `make lighthouse` would run an "
                       "unrelated npm package named `lhci`. See notes.")
        log("SKIPPED — @lhci/cli is not installed (see report notes)")
        return

    code, out = run(["make", "lighthouse"], timeout=600, env=node_env())
    score = None
    m = re.search(r"categories:performance[^\n]*?(\d\.\d{2})", out)
    if m:
        score = float(m.group(1))
    if score is None:
        for p in sorted(REPO.glob(".lighthouseci/lhr-*.json")):
            try:
                lhr = json.loads(p.read_text())
                score = lhr["categories"]["performance"]["score"]
            except Exception:                                # noqa: BLE001
                continue
    if score is None:
        rep.add("lighthouse", "Lighthouse performance",
                detail=f"unparsed (exit {code})")
        log("FAILED")
        return
    rep.add("lighthouse", "Lighthouse performance", f"{score:.2f}",
            "landing page, production build", ok=True)
    log(f"{score:.2f}")


# ── Metric: end-to-end latency + wire encoding (live) ─────────────────────────

def submit_pace(cfg):
    """Seconds to wait between orders from one client.

    The server runs a token bucket per connection (rate_limit.capacity tokens,
    refilled at refill_rate/sec). Submitting faster than the refill rate drains
    the burst, earns RATE_LIMIT_WARNING, and past suspend_threshold gets the
    connection suspended -- which shows up as a collapsed ack rate, not as an
    honest latency number. Stay at 80% of the refill rate.
    """
    try:
        rate = float(cfg["rate_limit"]["refill_rate"])
    except (KeyError, TypeError, ValueError):
        rate = 5.0
    return 1.0 / max(0.5, rate * 0.8)


async def metric_latency_and_wire(rep, keys, http, ws_url, orders, pace):
    section("Order -> ack latency, and wire encoding")

    # Clients 0 and 1 are observers: same lobby, same game mode, and neither
    # submits an order. That matters -- a trading client also receives
    # order_ack and other personalised frames, so comparing a trading msgpack
    # client against a trading JSON client compares two different message
    # mixes, not two encodings of the same stream.
    encodings = ["msgpack", "json"] + ["json"] * (PLAYERS_PER_LOBBY - 2)
    bundle = await open_lobby(keys[:PLAYERS_PER_LOBBY], http, ws_url, encodings)
    if not bundle:
        rep.add("latency", "Order -> ack (p50/p99)", detail="lobby setup failed")
        rep.add("wire", "msgpack vs JSON bytes", detail="lobby setup failed")
        log("FAILED")
        return

    clients, _, _, _ = bundle
    observers, timers = clients[:2], clients[2:]
    try:
        per_client = max(1, orders // len(timers))
        attempts = per_client * len(timers)
        log(f"submitting {attempts} orders across {len(timers)} clients "
            f"at {1/pace:.1f}/sec each "
            f"(~{attempts * pace / len(timers):.0f}s, "
            f"{len(observers)} idle observers for the wire comparison)")

        async def drive(c, n, seed):
            for i in range(n):
                if not c.in_round:
                    await asyncio.sleep(0.2)
                    continue
                suit = SUITS[(seed + i) % len(SUITS)]
                side = "buy" if (seed + i) % 2 == 0 else "sell"
                price = 20 + ((seed + i) % 40)
                await c.timed_order(suit, side, price)
                await asyncio.sleep(pace)

        await asyncio.gather(*(drive(c, per_client, i)
                               for i, c in enumerate(timers)))

        lat = sorted(x for c in timers for x in c.latencies_ms)
        errs = sum(c.errors for c in timers)
        if lat:
            p = lambda q: lat[min(len(lat) - 1, int(len(lat) * q))]   # noqa: E731
            rate = len(lat) / attempts * 100.0
            value = f"p50 {p(0.50):.2f} ms / p99 {p(0.99):.2f} ms"
            detail = (f"n={len(lat)} of {attempts} submitted ({rate:.0f}% acked, "
                      f"{errs} rejected), p95 {p(0.95):.2f} ms, "
                      f"mean {statistics.mean(lat):.2f} ms, "
                      f"max {lat[-1]:.2f} ms")
            # A thin sample is a weak number; say so rather than quoting a p99
            # computed from a handful of points.
            rep.add("latency", "Order -> ack (p50/p99)", value, detail,
                    ok=len(lat) >= 100)
            log(f"{value}  ({detail})")
        else:
            rep.add("latency", "Order -> ack (p50/p99)",
                    detail=f"no acks from {attempts} orders, {errs} rejected")
            log("no acks received")

        mp, js = observers[0], observers[1]
        if mp.md_frames and js.md_frames:
            skew = abs(mp.md_frames - js.md_frames) / max(mp.md_frames, js.md_frames)
            mp_avg = mp.md_bytes / mp.md_frames
            js_avg = js.md_bytes / js.md_frames
            delta = (mp_avg - js_avg) / js_avg * 100.0
            # Share of the whole stream this saving actually applies to -- the
            # honest framing, since most frames are JSON either way.
            share = js.md_bytes / js.bytes_in * 100.0 if js.bytes_in else 0.0
            value = f"{mp_avg:.0f} B vs {js_avg:.0f} B ({delta:+.0f}%)"
            detail = (f"market-data frames only, to idle observers; "
                      f"{mp.md_frames} msgpack / {js.md_frames} JSON of "
                      f"{js.frames_in} total; market data is {share:.0f}% of "
                      f"JSON-client bytes")
            if skew > 0.05:
                detail += f" — WARNING: {skew*100:.0f}% frame-count skew, not comparable"
            if mp.binary_frames == 0:
                detail += (" — WARNING: the msgpack socket received 0 binary "
                           "frames, so the subprotocol was negotiated but never "
                           "applied; this is a JSON-vs-JSON comparison")
            rep.add("wire", "msgpack vs JSON bytes", value, detail,
                    ok=skew <= 0.05 and mp.binary_frames > 0)
            log(f"{value}  ({detail})")
        else:
            rep.add("wire", "msgpack vs JSON bytes",
                    detail=f"no market-data frames "
                           f"(msgpack {mp.md_frames}, JSON {js.md_frames})")
            log("no market-data frames captured")
    finally:
        await teardown(bundle)


# ── Metric: capacity ramp + RSS (live) ────────────────────────────────────────

def server_rss_kb(ssh):
    """RSS of the server process in KB, locally or over SSH. None if not found.

    Matched on the binary path rather than the process name, because the binary
    is just called `server` and would collide with anything else running.
    """
    cmd = ("ps -o rss= -p \"$(pgrep -f 'server/server --config' | head -1)\" "
           "2>/dev/null | head -1")
    code, out = run(["ssh", ssh, cmd] if ssh else cmd, timeout=30)
    out = out.strip().splitlines()[0].strip() if out.strip() else ""
    return int(out) if out.isdigit() else None


def server_cpu_pct(ssh, window=2.0):
    """Server CPU as a percentage of ONE core, sampled over `window` seconds.

    Deliberately not `ps -o %cpu`, which averages over the whole process
    lifetime and would read low after an idle startup. This diffs utime+stime
    out of /proc/<pid>/stat across the window, which is what the load actually
    costs right now.
    """
    script = (
        "p=$(pgrep -f 'server/server --config' | head -1); [ -z \"$p\" ] && exit 1; "
        "a=$(awk '{print $14+$15}' /proc/$p/stat); "
        f"sleep {window}; "
        "b=$(awk '{print $14+$15}' /proc/$p/stat); "
        "echo $a $b"
    )
    code, out = run(["ssh", ssh, script] if ssh else script, timeout=int(window) + 30)
    parts = out.strip().split()
    if code != 0 or len(parts) != 2:
        return None
    try:
        ticks = int(parts[1]) - int(parts[0])
    except ValueError:
        return None
    hz = 100.0                       # USER_HZ is 100 on every Linux we target
    return ticks / hz / window * 100.0


async def metric_capacity_and_rss(rep, seeder, http, ws_url, max_lobbies, ssh, pace):
    section("Capacity ramp and RSS")

    baseline = server_rss_kb(ssh)
    cpu_baseline = server_cpu_pct(ssh)
    if baseline is None:
        log("server RSS unreadable (need local run or --ssh); RSS metric skipped")
    if cpu_baseline is not None:
        log(f"idle: {baseline/1024:.0f} MB, {cpu_baseline:.1f}% of one core")

    open_bundles = []
    healthy = 0
    samples = []
    try:
        for n in range(1, max_lobbies + 1):
            need = PLAYERS_PER_LOBBY
            try:
                keys = seeder.seed(need)
            except RuntimeError as e:
                log(f"seeding stopped at lobby {n}: {e}")
                break

            bundle = await open_lobby(keys, http, ws_url)
            if not bundle:
                log(f"lobby {n} failed to open — stopping ramp")
                break
            open_bundles.append(bundle)

            # Probe this step's health from the newest lobby.
            clients = bundle[0]
            probe = clients[0]
            for i in range(25):
                await probe.timed_order(SUITS[i % 4], "buy" if i % 2 else "sell",
                                        20 + (i % 40))
                await asyncio.sleep(pace)

            lat = sorted(probe.latencies_ms)
            if not lat:
                log(f"{n} lobbies: no acks — stopping ramp")
                break
            p99 = lat[min(len(lat) - 1, int(len(lat) * 0.99))]
            rss = server_rss_kb(ssh)
            cpu = server_cpu_pct(ssh)
            samples.append((n, p99, rss, cpu))
            line = f"{n:>3} lobbies  p99 {p99:6.2f} ms"
            if rss:
                line += f"  rss {rss/1024:.0f} MB"
            if cpu is not None:
                line += f"  cpu {cpu:5.1f}% of one core"
            log(line)

            if p99 > ACK_P99_BUDGET_MS:
                log(f"p99 exceeded {ACK_P99_BUDGET_MS} ms budget — stopping ramp")
                break
            healthy = n

        if healthy:
            last = next((s for s in samples if s[0] == healthy), None)
            # If the ramp never degraded, `healthy` is just --max-lobbies. Say
            # so: a ceiling the test never reached is a lower bound, not a max.
            capped = healthy >= max_lobbies
            value = f"≥{healthy}" if capped else str(healthy)
            detail = f"{healthy * PLAYERS_PER_LOBBY} concurrent players"
            if last and last[1]:
                detail += f", ack p99 {last[1]:.2f} ms"
            detail += (" — ramp ceiling reached without degrading, raise "
                       "--max-lobbies for the true limit" if capped
                       else f", degraded past {ACK_P99_BUDGET_MS:.0f} ms budget")
            rep.add("capacity", "Max concurrent lobbies", value, detail, ok=True)
        else:
            rep.add("capacity", "Max concurrent lobbies", detail="ramp failed at 1")

        rss_samples = [(n, r) for n, _, r, _ in samples if r]
        if baseline and rss_samples:
            n, r = rss_samples[-1]
            per = (r - baseline) / n / 1024.0
            rep.add("rss", "Server RSS per lobby", f"{per:.1f} MB",
                    f"baseline {baseline/1024:.0f} MB, "
                    f"{r/1024:.0f} MB at {n} lobbies", ok=True)
            log(f"{per:.1f} MB per lobby")
        else:
            rep.add("rss", "Server RSS per lobby", detail="RSS unreadable")

        cpu_samples = [(n, c) for n, _, _, c in samples if c is not None]
        if cpu_samples:
            n, c = cpu_samples[-1]
            per_cpu = (c - (cpu_baseline or 0.0)) / n
            rep.add("cpu", "Server CPU per lobby", f"{per_cpu:.2f}% of one core",
                    f"idle {cpu_baseline:.1f}%, {c:.1f}% at {n} lobbies "
                    f"({n * PLAYERS_PER_LOBBY} players)"
                    if cpu_baseline is not None else
                    f"{c:.1f}% of one core at {n} lobbies", ok=True)
            log(f"{per_cpu:.2f}% of one core per lobby")
        else:
            rep.add("cpu", "Server CPU per lobby", detail="CPU unreadable")
    finally:
        for b in open_bundles:
            await teardown(b)


# ── Reporting ─────────────────────────────────────────────────────────────────

def emit(rep, out_path):
    width = max(len(m.label) for m in rep.metrics) + 2
    print("\n" + "=" * 72)
    print(f"\033[1mANJEER METRICS\033[0m — target: {rep.target}")
    print("=" * 72)
    for m in rep.metrics:
        mark = "\033[32m✓\033[0m" if m.ok else "\033[31m✗\033[0m"
        print(f"{mark} {m.label:<{width}} {m.value}")
        if m.detail:
            print(f"  {'':<{width}} \033[2m{m.detail}\033[0m")
    print("=" * 72)

    lines = [
        f"# Anjeer metrics — {date.today().isoformat()}",
        "",
        f"Target: `{rep.target}`  ",
        f"Collected: {time.strftime('%Y-%m-%d %H:%M:%S %Z')}",
        "",
        "| Metric | Value | Detail |",
        "|---|---|---|",
    ]
    for m in rep.metrics:
        detail = m.detail.replace("|", "\\|").replace("\n", " ")
        lines.append(f"| {m.label} | {m.value} | {detail} |")
    lines += ["", "Users (total / daily active) are recorded separately.", ""]
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines))
    print(f"\nWritten to {out_path.relative_to(REPO)}")


# ── Entry point ───────────────────────────────────────────────────────────────

async def amain(args):
    cfg = load_config(args.target)
    http, ws_url = endpoints(cfg, args.target, args.http, args.ws)
    rep = Report(target=f"{args.target} ({http})")

    skip = set(args.skip.split(",")) if args.skip else set()
    offline_only = args.offline

    if "orderbook" not in skip:
        metric_orderbook(rep, args.perf_n)
    if "render" not in skip:
        metric_render(rep)
    if "lighthouse" not in skip:
        metric_lighthouse(rep)

    live = {"latency", "wire", "capacity", "rss"} - skip
    if live and not offline_only and not require_websockets():
        log("websockets not installed (pip install websockets) — skipping live metrics")
        live = set()
    if live and not offline_only:
        code, _ = run(["curl", "-sf", "-m", "5", f"{http}/health"], timeout=20)
        if code != 0:
            log(f"server not healthy at {http} — skipping live metrics")
            for k, lbl in [("latency", "Order -> ack (p50/p99)"),
                           ("wire", "msgpack vs JSON bytes"),
                           ("capacity", "Max concurrent lobbies"),
                           ("rss", "Server RSS per lobby")]:
                if k in live:
                    rep.add(k, lbl, detail="server unreachable")
        else:
            seeder = Seeder(cfg["db"]["connection_string"])
            if not seeder.available():
                log("psql not on PATH — skipping live metrics")
            else:
                pace = submit_pace(cfg)
                try:
                    if {"latency", "wire"} & live:
                        keys = seeder.seed(PLAYERS_PER_LOBBY)
                        await metric_latency_and_wire(rep, keys, http, ws_url,
                                                      args.orders, pace)
                    if {"capacity", "rss"} & live:
                        await metric_capacity_and_rss(rep, seeder, http, ws_url,
                                                      args.max_lobbies, args.ssh,
                                                      pace)
                finally:
                    log("cleaning up seeded rows")
                    seeder.cleanup()

    out = Path(args.out) if args.out else \
        REPO / "docs" / f"metrics-{date.today().isoformat()}.md"
    emit(rep, out)


def main():
    ap = argparse.ArgumentParser(description="Collect all Anjeer perf metrics.")
    ap.add_argument("--target", choices=["local", "prod"], default="local")
    ap.add_argument("--http", help="override HTTP base URL")
    ap.add_argument("--ws", help="override WebSocket URL")
    ap.add_argument("--ssh", help="ssh host for reading prod RSS, e.g. ec2-user@1.2.3.4")
    ap.add_argument("--orders", type=int, default=600, help="orders for the latency sample")
    ap.add_argument("--max-lobbies", type=int, default=12, help="capacity ramp ceiling")
    ap.add_argument("--perf-n", type=int, default=200000, help="PERF_N for the C++ bench")
    ap.add_argument("--offline", action="store_true", help="skip everything needing a server")
    ap.add_argument("--skip", help="comma-separated metric keys to skip")
    ap.add_argument("--out", help="report path (default docs/metrics-<date>.md)")
    args = ap.parse_args()

    try:
        asyncio.run(amain(args))
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        sys.exit(130)


if __name__ == "__main__":
    main()
