#pragma once

#include "engine/engine.h"
#include "exchange/exchange_session.h"
#include "server/eval/eval_types.h"

#include <array>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace anjeer::server {

// ── Inbound: network thread → game-loop thread ────────────────────────────────

// Minimal admission info for NetAdmitQueue. WsHandle is omitted: GameSession
// does not own sockets. WsServer updates its own handle maps independently.
struct SlotAdmitInfo {
    int         slot_index;
    int64_t     player_id;
    std::string username;
};

struct NetConnect    { int32_t slot; int64_t player_id; std::string username; };
struct NetDisconnect { int32_t slot; };
struct NetSubmit     { int32_t slot; std::string suit; engine::Side side; int32_t price; };
struct NetNudge      { int32_t slot; std::string suit; engine::Side side; };
struct NetCancel     { int32_t slot; int64_t order_id; };
struct NetStartGame     {};
// Owner explicitly starts the next round during the inter-round window, bypassing
// the auto-start countdown. Only WsServer enqueues this after verifying the sender
// is the current owner (current_owner_player_id_ in ActiveSession).
struct NetOwnerStartRound {};
// Owner force-ends the game during the inter-round window.
struct NetOwnerEndGame {};
// Permanent leave: player sent leave_lobby during an active session. Unlike
// NetDisconnect (temporary drop), this marks the slot inactive and decrements
// active_player_count_ so check_end_condition can fire.
struct NetPermanentLeave { int32_t slot; };
// Spectator join/leave — routed to GameSession so Task 8 can emit a state
// snapshot targeted at the new spectator without WsServer knowing game state.
struct NetSpectatorJoin  { int32_t spectator_id; std::string spectator_name; };
struct NetSpectatorLeave { int32_t spectator_id; };

// Reconnect-aware disconnect: starts the per-slot reconnect window timer in
// GameSession. WsServer calls handle_player_disconnect() which enqueues this.
// Distinct from NetDisconnect so the two flows (legacy close vs. reattach-aware
// close) can coexist until T10 migrates WsServer entirely to the new path.
struct NetReconnectDisconnect { int32_t slot; };

// Carries the fresh token WsServer created BEFORE enqueuing so that the
// game-loop thread can embed it in the state snapshot without a DB call.
struct NetReconnectReattach {
    int32_t     slot;
    std::string reconnect_token;
    int64_t     reconnect_expires_at_ms;
};

// Routes queue admission through the SPSC inbound queue so GameSession's
// slots_ is mutated on the game-loop thread only. WsServer enqueues this when
// it handles GameRoundStarted; GameSession processes it before the next tick.
struct NetAdmitQueue { std::vector<SlotAdmitInfo> entries; };

struct NetSendFeedSnapshot { int32_t slot; std::string tier; }; // "mbpn" | "mbo"

// Triggers an MBO on-connect snapshot targeted at a /ws/marketdata connection.
// md_id is assigned by WsServer from a per-session counter; not a player slot.
struct NetMarketDataConnect { int32_t md_id; };

using NetEvent = std::variant<
    NetConnect, NetDisconnect,
    NetSubmit, NetNudge, NetCancel,
    NetStartGame, NetOwnerStartRound, NetOwnerEndGame, NetPermanentLeave,
    NetSpectatorJoin, NetSpectatorLeave,
    NetReconnectDisconnect, NetReconnectReattach,
    NetAdmitQueue, NetSendFeedSnapshot, NetMarketDataConnect>;

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

// Signals WsServer to spawn a replacement bot for a slot vacated mid-round.
// Always emitted by GameSession::handle_permanent_leave when a real player leaves
// during RoundActive. WsServer decides whether to act on it based on lobby policy
// (ActiveSession::spawn_bots_on_leave). Difficulty is also supplied by WsServer.
struct GameSpawnBot {
    int                slot;
    std::array<int, 4> hand;        // card counts per suit at departure
    int32_t            balance;     // departing player's last balance
    float              remaining_s; // seconds left in the current round
};

// Outbound signal: reconnect window expired for this slot.
// WsServer sends reconnect_window_expired to the client socket (if it is still
// connected from a previous session) and removes any pending reconnect token.
struct GameReconnectExpired { int32_t slot; };

// Outbound signal: a new round just entered begin_round().
// AGENT-CTX: WsServer handles this to drain LobbyQueue entries into available
// slots. Emitted before round_start payloads so admitted players are wired up
// before the game-loop thread sends targeted round_start messages.
struct GameRoundStarted {};

// Eval output routed back through the SPSC outbound queue so WsServer can
// deliver it to the correct WS handles on the event-loop thread.
// target_slot == -1 → broadcast to all active + spectator handles.
struct GameEvalOutput { eval::EvalOutput out; };

struct GameBookUpdate {
    std::string                               mbp1_json;
    engine::Suit                              suit;
    std::vector<exchange::PriceLevel>         bids;
    std::vector<exchange::PriceLevel>         asks;
    exchange::seq_t                           seq;
};

// Pre-serialized MBO incremental event (order_added / order_executed /
// order_cancelled). WsServer fans this out to sockets with FeedTier::MBO only.
struct GameMboEvent { std::string json; };

// Targeted delivery to one /ws/marketdata connection (identified by md_id,
// not a player slot). Used to deliver the on-connect MBO snapshot.
struct GameMarketDataTargeted { int32_t md_id; std::string json; };

using GameEvent = std::variant<GameBroadcast, GameTargeted, GameDone,
                               GameSpectatorTargeted, GameSpectatorBroadcast,
                               GameSpawnBot, GameReconnectExpired,
                               GameRoundStarted, GameEvalOutput,
                               GameBookUpdate, GameMboEvent,
                               GameMarketDataTargeted>;

} // namespace anjeer::server
