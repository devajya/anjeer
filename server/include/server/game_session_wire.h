#pragma once

// Wire protocol helpers for GameSession — JSON builders and parsers.
// No game state; no side effects beyond socket I/O in the send functions.
// Editing the wire format requires changes only in this header and game_session_wire.cpp.

#include "server/game_session.h"
#include "server/logger.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace anjeer::server {

// Exhaustive engine→wire code translation. Adding a new engine error code
// causes a compile error until the server explicitly handles it.
WsErrorCode to_ws_error_code(engine::OrderErrorEvent::Code c) noexcept;

// ═══════════════════════════════════════════════════════════════════════════
// namespace parse — JSON → typed-command layer
//
// Pure parsing — no socket I/O, no game-state access. Each function returns
// std::nullopt on any field error; the caller sends the wire error.
// ═══════════════════════════════════════════════════════════════════════════
namespace parse {

struct SubmitOrderFields { std::string suit; std::string side; int32_t price; };
struct NudgeFields        { std::string suit; std::string side; };
struct CancelFields       { int64_t order_id; };

std::optional<SubmitOrderFields> submit_order(const nlohmann::json& j);
std::optional<NudgeFields>       nudge        (const nlohmann::json& j);
std::optional<CancelFields>      cancel_order (const nlohmann::json& j);
std::optional<engine::Side>      side         (const std::string& s) noexcept;

} // namespace parse

// ═══════════════════════════════════════════════════════════════════════════
// namespace serialise — event → JSON and socket-send layer
//
// Pure output — converts data to wire format and pushes bytes to sockets.
// No game-state reads or writes.
// ═══════════════════════════════════════════════════════════════════════════
namespace serialise {

std::string error_code_str(WsErrorCode c) noexcept;
std::string side           (engine::Side s) noexcept;
std::string opt_price      (std::optional<int32_t> p) noexcept;

// Shared between on-connect snapshot and broadcast_book_update so the wire
// format stays in sync regardless of call site.
std::string book_update_payload(const std::string&     suit,
                                 std::optional<int32_t> best_bid,
                                 std::optional<int32_t> best_ask);

std::string round_end_payload  (const engine::RoundResult& result,
                                 const std::vector<int>&    available_cash);
std::string round_start_payload(int                       slot,
                                 const engine::PlayerHand& hand,
                                 const std::string&        round_end_at,
                                 int                       effective_balance);

void error      (WsHandle ws, WsErrorCode code, std::string_view msg, Logger& slog);

// best_bid / best_ask sent as JSON null when nullopt (no resting orders).
// Clients must handle null on both fields — happens after a wipe.
void book_update(const std::set<WsHandle>& conns,
                 const std::string&        suit,
                 std::optional<int32_t>    best_bid,
                 std::optional<int32_t>    best_ask,
                 Logger&                   slog);

// your_side is null for spectators (anyone who is neither buyer nor seller).
void trade      (const std::set<WsHandle>& conns,
                 const engine::TradeEvent& t,
                 Logger&                   slog);

} // namespace serialise

} // namespace anjeer::server
