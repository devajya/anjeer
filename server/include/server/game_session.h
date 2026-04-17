#pragma once

namespace anjeer::server {

// AGENT-CTX: Describes which protocol messages are accepted — pure server-layer
// policy, not game logic. Exposed here so future HTTP endpoints (/api/status)
// and LobbyManager (Slice 6) can inspect phase without depending on ws_server.cpp.
enum class RoundPhase { Waiting, Active, Scoring, Ended };

} // namespace anjeer::server
