#pragma once

// AGENT-CTX: ws_types.h exists because Slice 7 decoupled GameSession from uWS.
// PerSocketData, WsHandle, and WsErrorCode were originally in game_session.h.
// Moving them here breaks the circular dependency:
//   game_session_wire.h needs WsHandle/WsErrorCode (for legacy send functions)
//   game_session.h (new) must NOT include uWS (GameSession runs on its own thread)
// Both game_session_wire.h and ws_server.cpp include this file directly.

#include <App.h>
#include <cstdint>
#include <optional>
#include <string>

namespace anjeer::server {

enum class ConnectionRole { Player, Spectator };
enum class AuthType       { JWT, ApiKey };

// AGENT-CTX: Complete wire-protocol error code set for the WS channel.
// Engine codes reach here via to_ws_error_code() in game_session_wire.cpp.
// Server-layer codes (UnknownSuit, MalformedMessage, etc.) are referenced
// directly at each call site. Never add game-logic meaning to these codes.
// Defined before PerSocketData so pending_close can use std::optional<WsErrorCode>.
enum class WsErrorCode {
    PriceOutOfRange,
    OrderNotFound,
    NotYourOrder,
    UnknownSuit,
    MalformedMessage,
    ServerFull,
    RoundNotActive,
    InsufficientBalance,
    // Slice 9 additions
    ApiKeyInvalid,
    LobbyModeMismatch,
    SpectatorNotAllowed,
    RateLimitWarning,
    RateLimitExceeded,
};

struct PerSocketData {
    int32_t        player_slot = -1;
    int64_t        player_id   = -1;
    std::string    lobby_id;
    std::string    username;
    ConnectionRole role      = ConnectionRole::Player;
    AuthType       auth_type = AuthType::JWT;
    // Set in .upgrade when auth fails or player is suspended; checked in .open
    // to send a typed error and close before the connection is used.
    std::optional<WsErrorCode> pending_close;
};

using WsHandle = uWS::WebSocket<false, true, PerSocketData>*;

} // namespace anjeer::server
