"""
Anjeer Python script template — websockets + asyncio

HOW TO USE
----------
1. Run `anjeer setup` once to store your API key and server URLs.
2. Run `anjeer join <CODE>` or `anjeer start <CODE>` to enter a lobby.
   The CLI joins the lobby for you, then launches this script with the
   correct environment variables already set. Do NOT call the join
   HTTP endpoint from this script — the lobby status will already be
   Starting or InGame and the request will return 409 GAME_ALREADY_STARTED.
3. Fill in your strategy inside the handler functions below.
4. This template does nothing strategically — it only logs every event.

ENVIRONMENT VARIABLES (set by the CLI before your script runs)
--------------------------------------------------------------
  ANJEER_API_KEY         — Bearer token for WebSocket auth
  ANJEER_SERVER_WS_URL   — WebSocket URL, e.g. ws://localhost:9001/ws
  ANJEER_HTTP_URL        — HTTP base URL, e.g. http://localhost:8080
  ANJEER_LOBBY_CODE      — Lobby code your script is participating in
  ANJEER_GAME_MODE       — Game mode: simple | intermediate | advanced
                           Determines which market-data messages you receive:
                             simple       → book_update (MBP-1, best bid/ask only)
                             intermediate → book_depth + book_depth_snapshot (MBP-N)
                             advanced     → order_added/executed/cancelled/book_snapshot (MBO)

DEPENDENCIES
------------
  pip install websockets
"""

import asyncio
import json
import os
import signal
import sys

import websockets

# ── Configuration ─────────────────────────────────────────────────────────────

API_KEY    = os.environ.get("ANJEER_API_KEY")
LOBBY_CODE = os.environ.get("ANJEER_LOBBY_CODE")
GAME_MODE  = os.environ.get("ANJEER_GAME_MODE", "simple")
_ws        = os.environ.get("ANJEER_SERVER_WS_URL", "ws://localhost:9001/ws")

if not API_KEY:
    print("ERROR: ANJEER_API_KEY not set. Launch via `anjeer join` or set the env var.")
    sys.exit(1)
if not LOBBY_CODE:
    print("ERROR: ANJEER_LOBBY_CODE not set. Launch via `anjeer join` or set the env var.")
    sys.exit(1)

# ── State ─────────────────────────────────────────────────────────────────────

player_id:     int | None = None
my_slot:       int | None = None
hand:          dict = {}
balance:       float | None = None
my_orders:     dict = {}   # order_id → {suit, side, price, qty_remaining}
in_round:      bool = False
rounds_played: int = 0

# MBP-1 / MBP-N state (simple + intermediate)
books:         dict = {}   # suit → {best_bid, best_ask, ...}
depth:         dict = {}   # suit → {bids: [...], asks: [...]}  (intermediate only)

# MBO state (advanced only)
order_book:    dict = {}   # order_id → {suit, side, price, qty_remaining, player_slot}

ws_ref = None
_shutdown = asyncio.Event()

# ── WS helpers ────────────────────────────────────────────────────────────────

async def submit_order(suit: str, side: str, price: int, qty: int = 1):
    await ws_ref.send(json.dumps({"type": "submit_order", "suit": suit, "side": side, "price": price, "qty": qty}))

async def nudge(suit: str, side: str):
    await ws_ref.send(json.dumps({"type": "nudge", "suit": suit, "side": side}))

async def cancel_order(order_id: int):
    await ws_ref.send(json.dumps({"type": "cancel_order", "order_id": order_id}))

async def vote_to_end():
    await ws_ref.send(json.dumps({"type": "vote_to_end"}))

async def script_log(message: str):
    """Send a log line visible to spectators watching this lobby."""
    await ws_ref.send(json.dumps({"type": "script_log", "message": message}))

# ── Shared handlers (all modes) ───────────────────────────────────────────────

async def on_player_hello(msg: dict):
    global player_id
    player_id = msg["player_id"]
    print(f"[hello] player_id={player_id} game_mode={GAME_MODE}")

async def on_round_start(msg: dict):
    global hand, balance, my_slot, in_round, my_orders, books, depth, order_book
    hand      = msg["hand"]
    balance   = msg["balance"]
    my_slot   = msg["player_slot"]
    in_round  = True
    my_orders = {}
    books     = {}
    depth     = {}
    order_book = {}
    print(f"[round_start] slot={my_slot} hand={hand} balance={balance:.2f} mode={msg.get('game_mode', GAME_MODE)}")
    # TODO: add your opening strategy here

async def on_trade(msg: dict):
    global my_orders
    # In simple/intermediate mode, wipe_on_trade=true: all books clear after every trade.
    # In advanced mode, wipe_on_trade=false: only the matched orders are removed.
    if GAME_MODE != "advanced":
        my_orders = {}
    print(f"[trade] suit={msg['suit']} price={msg['price']} your_side={msg.get('your_side')}")
    # TODO: re-post standing orders if desired (simple/intermediate only)

async def on_order_ack(msg: dict):
    my_orders[msg["order_id"]] = {
        "suit": msg["suit"], "side": msg["side"],
        "price": msg["price"], "qty_remaining": msg.get("qty_remaining", 1),
    }
    print(f"[ack] {msg['side']} {msg['suit']}@{msg['price']} qty={msg.get('qty_remaining',1)} id={msg['order_id']}")

async def on_order_cancel_ack(msg: dict):
    my_orders.pop(msg["order_id"], None)

async def on_order_partially_filled(msg: dict):
    """Advanced mode only: fired when your resting order is partially filled."""
    oid = msg["order_id"]
    qty_remaining = msg.get("qty_remaining", 0)
    if oid in my_orders:
        my_orders[oid]["qty_remaining"] = qty_remaining
    print(f"[partial_fill] id={oid} filled={msg.get('qty_filled',1)} remaining={qty_remaining}")
    # TODO: decide whether to re-price or let the remainder rest

async def on_round_end(msg: dict):
    global in_round, my_orders, rounds_played
    in_round      = False
    my_orders     = {}
    rounds_played += 1
    print(f"[round_end] goal={msg['goal_suit']} (rounds played: {rounds_played})")
    for r in msg["results"]:
        print(f"  slot={r['player_slot']} payout={r['payout']} balance={r['balance']}")

async def on_inter_round(msg: dict):
    print(f"[inter_round] round={msg.get('round_number')} next in {msg.get('inter_round_seconds')}s")
    # TODO: call await vote_to_end() here if you want to end the game

async def on_game_ended(msg: dict):
    print("[game_ended]")
    for r in msg.get("final_standings", []):
        print(f"  slot={r['player_slot']} balance={r['balance']}")
    _shutdown.set()

async def on_error(msg: dict):
    print(f"[error] {msg['code']}: {msg['message']}")

async def on_unknown(msg: dict):
    print(f"[?] {msg.get('type')} {msg}")

# ── MBP-1 handlers (simple mode) ─────────────────────────────────────────────

async def on_book_update(msg: dict):
    """MBP-1: top-of-book update. Fired after every order event in simple mode."""
    suit = msg["suit"]
    books[suit] = {
        "best_bid":      msg["best_bid"],
        "best_ask":      msg["best_ask"],
        "best_bid_slot": msg["best_bid_slot"],
        "best_ask_slot": msg["best_ask_slot"],
    }
    # TODO: react to price changes

# ── MBP-N handlers (intermediate mode) ───────────────────────────────────────

async def on_book_depth(msg: dict):
    """MBP-N: N-level price ladder update for one side of one suit."""
    suit = msg["suit"]
    side = msg["side"]   # "bid" or "ask"
    if suit not in depth:
        depth[suit] = {"bids": [], "asks": []}
    depth[suit][side + "s"] = msg["levels"]   # [{price, qty}, ...]
    # TODO: compute fair value or liquidity from the depth

async def on_book_depth_snapshot(msg: dict):
    """MBP-N: full N-level snapshot broadcast at round start."""
    suit = msg["suit"]
    depth[suit] = {
        "bids": msg.get("bids", []),
        "asks": msg.get("asks", []),
    }

# ── MBO handlers (advanced mode) ─────────────────────────────────────────────

async def on_order_added(msg: dict):
    """MBO: a new resting order is visible in the book."""
    oid = msg["order_id"]
    order_book[oid] = {
        "suit":          msg["suit"],
        "side":          msg["side"],
        "price":         msg["price"],
        "qty_remaining": msg.get("qty_remaining", 1),
        "player_slot":   msg.get("player_slot"),
        "seq":           msg.get("seq"),
    }
    # TODO: update your order-book model

async def on_order_executed(msg: dict):
    """MBO: a resting order was fully or partially filled."""
    oid = msg["order_id"]
    qty_remaining = msg.get("qty_remaining", 0)
    if qty_remaining == 0:
        order_book.pop(oid, None)
    elif oid in order_book:
        order_book[oid]["qty_remaining"] = qty_remaining
    print(f"[executed] id={oid} price={msg['price']} qty_filled={msg.get('qty_filled',1)}")

async def on_order_cancelled(msg: dict):
    """MBO: a resting order was cancelled."""
    order_book.pop(msg["order_id"], None)

async def on_order_book_snapshot(msg: dict):
    """MBO: full book snapshot broadcast at round start."""
    global order_book
    order_book = {}
    for entry in msg.get("orders", []):
        order_book[entry["order_id"]] = {
            "suit":          entry["suit"],
            "side":          entry["side"],
            "price":         entry["price"],
            "qty_remaining": entry.get("qty_remaining", 1),
            "player_slot":   entry.get("player_slot"),
        }

# ── Dispatch ──────────────────────────────────────────────────────────────────

HANDLERS = {
    # ── shared ────────────────────────────────────────────────────────────────
    "player_hello":          on_player_hello,
    "trade":                 on_trade,
    "order_ack":             on_order_ack,
    "order_cancel_ack":      on_order_cancel_ack,
    "order_partially_filled": on_order_partially_filled,
    "round_starting":        lambda m: print(f"[countdown] starts_at={m['starts_at']}"),
    "round_start":           on_round_start,
    "round_end":             on_round_end,
    "inter_round":           on_inter_round,
    "game_ended":            on_game_ended,
    "error":                 on_error,
    "waiting_for_start":     lambda m: print(f"[waiting] {m['connected']}/{m['required']} players"),
    "vote_tally":            lambda m: print(f"[vote_tally] {m['votes']}/{m['required']}"),
    "all_balances":          lambda m: None,
    "hand_totals":           lambda m: None,
    "delta_update":          lambda m: None,
    "game_player_left":      lambda m: print(f"[player_left] slot={m['player_slot']}"),
    "session_error":         lambda m: print(f"[session_error] {m.get('message')}") or _shutdown.set(),
    "spectator_count":       lambda m: None,
    "heartbeat":             lambda m: None,
    # ── MBP-1 (simple) ────────────────────────────────────────────────────────
    "book_update":           on_book_update,
    # ── MBP-N (intermediate) ──────────────────────────────────────────────────
    "book_depth":            on_book_depth,
    "book_depth_snapshot":   on_book_depth_snapshot,
    # ── MBO (advanced) ────────────────────────────────────────────────────────
    "order_added":           on_order_added,
    "order_executed":        on_order_executed,
    "order_cancelled":       on_order_cancelled,
    "order_book_snapshot":   on_order_book_snapshot,
}

# ── Main ──────────────────────────────────────────────────────────────────────

async def recv_loop(ws):
    async for raw in ws:
        if _shutdown.is_set():
            break
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            print(f"[warn] non-JSON: {raw!r}")
            continue
        t = msg.get("type", "")
        handler = HANDLERS.get(t, on_unknown)
        if asyncio.iscoroutinefunction(handler):
            await handler(msg)
        else:
            handler(msg)

async def main():
    global ws_ref

    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, _shutdown.set)

    # The CLI has already joined the lobby and started the game before
    # launching this script. Connect directly — do not POST to /lobbies/join.
    headers = {"Authorization": f"Bearer {API_KEY}"}
    print(f"Connecting to {_ws} (lobby {LOBBY_CODE}, mode={GAME_MODE}) …")
    async with websockets.connect(_ws, additional_headers=headers) as ws:
        ws_ref = ws
        print("Connected. Waiting for player_hello …")
        recv     = asyncio.create_task(recv_loop(ws))
        shutdown = asyncio.create_task(_shutdown.wait())
        done, pending = await asyncio.wait(
            {recv, shutdown}, return_when=asyncio.FIRST_COMPLETED
        )
        for t in pending:
            t.cancel()
    print("\nDisconnected.")

if __name__ == "__main__":
    asyncio.run(main())
