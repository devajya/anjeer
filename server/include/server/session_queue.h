#pragma once

#include "engine/engine.h"

#include <string>
#include <variant>

namespace anjeer::server {

// ── Inbound: network thread → game-loop thread ────────────────────────────────

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
// Spectator join/leave — routed to GameSession so Task 8 can emit a state
// snapshot targeted at the new spectator without WsServer knowing game state.
struct NetSpectatorJoin  { int32_t spectator_id; std::string spectator_name; };
struct NetSpectatorLeave { int32_t spectator_id; };

using NetEvent = std::variant<
    NetConnect, NetDisconnect,
    NetSubmit, NetNudge, NetCancel,
    NetStartGame, NetVoteToEnd, NetPermanentLeave,
    NetSpectatorJoin, NetSpectatorLeave>;

// ── Outbound: game-loop thread → network thread ───────────────────────────────
// AGENT-CTX: GameSession never touches WsHandle or uWS directly — it writes
// GameEvent variants into the outbound queue. WsServer drains the queue on a
// 16ms uWS timer and does the actual ws->send() calls. This keeps the game loop
// thread free of any uWS dependency and makes the threading model explicit.

struct GameBroadcast { std::string json; };           // send to all connected slots
struct GameTargeted  { int32_t slot; std::string json; }; // send to one slot
struct GameDone      {};                              // session ended; WsServer tears down
// Targeted delivery to one spectator; used by Task 8 snapshot on join.
struct GameSpectatorTargeted  { int32_t spectator_id; std::string json; };
struct GameSpectatorBroadcast { std::string json; };  // send to all spectators only

using GameEvent = std::variant<GameBroadcast, GameTargeted, GameDone, GameSpectatorTargeted, GameSpectatorBroadcast>;

} // namespace anjeer::server
