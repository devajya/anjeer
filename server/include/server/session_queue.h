#pragma once

#include "engine/engine.h"

#include <string>
#include <variant>

namespace anjeer::server {

// ── Inbound: network thread → game-loop thread ────────────────────────────────
// AGENT-CTX: Each NetEvent variant carries only the data needed for that event —
// no shared pointers, no heap allocation. The SPSC queue copies the variant by
// value, which is safe and cheap given the small payload sizes here.

struct NetConnect    { int32_t slot; int64_t player_id; std::string username; };
struct NetDisconnect { int32_t slot; };
struct NetSubmit     { int32_t slot; std::string suit; engine::Side side; int32_t price; };
struct NetNudge      { int32_t slot; std::string suit; engine::Side side; };
struct NetCancel     { int32_t slot; int64_t order_id; };
struct NetStartGame     {};
struct NetVoteToEnd     { int32_t slot; };
// Permanent leave: player sent leave_lobby during an active session. Unlike
// NetDisconnect (temporary drop), this marks the slot inactive and decrements
// active_player_count_ so check_end_condition can fire.
struct NetPermanentLeave { int32_t slot; };

using NetEvent = std::variant<
    NetConnect, NetDisconnect,
    NetSubmit, NetNudge, NetCancel,
    NetStartGame, NetVoteToEnd, NetPermanentLeave>;

// ── Outbound: game-loop thread → network thread ───────────────────────────────
// AGENT-CTX: GameSession never touches WsHandle or uWS directly — it writes
// GameEvent variants into the outbound queue. WsServer drains the queue on a
// 16ms uWS timer and does the actual ws->send() calls. This keeps the game loop
// thread free of any uWS dependency and makes the threading model explicit.

struct GameBroadcast { std::string json; };           // send to all connected slots
struct GameTargeted  { int32_t slot; std::string json; }; // send to one slot
struct GameDone      {};                              // session ended; WsServer tears down

using GameEvent = std::variant<GameBroadcast, GameTargeted, GameDone>;

} // namespace anjeer::server
