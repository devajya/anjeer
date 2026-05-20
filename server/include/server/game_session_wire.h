#pragma once

// Wire protocol helpers for GameSession — JSON builders and parsers.
// No game state; no side effects beyond socket I/O in the send functions.
// Editing the wire format requires changes only in this header and game_session_wire.cpp.

// AGENT-CTX: game_session_wire.h includes ws_types.h (not game_session.h) since
// Slice 7. The new GameSession has no uWS dependency, so WsHandle/WsErrorCode
// live in ws_types.h. The legacy send functions (error, book_update, trade) that
// take WsHandle are still used by ws_server.cpp directly during the Slice 7→8
// transition; they will be removed when Task 8 completes the WsServer refactor.
#include "server/ws_types.h"
#include "server/logger.h"
#include "engine/engine.h"
#include <nlohmann/json.hpp>

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
struct LeaveLobbyFields   { std::string lobby_id; };

std::optional<SubmitOrderFields> submit_order(const nlohmann::json& j);
std::optional<NudgeFields>       nudge        (const nlohmann::json& j);
std::optional<CancelFields>      cancel_order (const nlohmann::json& j);
std::optional<engine::Side>      side         (const std::string& s) noexcept;
std::optional<LeaveLobbyFields>  leave_lobby  (const nlohmann::json& j);

} // namespace parse

// ═══════════════════════════════════════════════════════════════════════════
// namespace serialise — event → JSON and socket-send layer
//
// Pure output — converts data to wire format and pushes bytes to sockets.
// No game-state reads or writes.
// ═══════════════════════════════════════════════════════════════════════════
// ── Payload structs for multi-field messages ─────────────────────────────────
// AGENT-CTX: Plain data structs live here (not inside GameSession) so the wire
// layer has no compile-time dependency on GameSession internals.  GameSession
// builds these from its own SlotInfo / RoundSummary types before calling the
// serialise functions below.  Renaming a GameSession field never forces a
// recompile of everything that includes this header.

struct WirePlayerResult {
    int  player_slot;
    int  goal_cards_held;
    int  payout;
    int  balance;
    bool disconnected;
};

struct WireRoundSummary {
    int                           round_number;
    std::string                   goal_suit;
    std::vector<WirePlayerResult> results;
};

struct WireFinalStanding {
    int         player_slot;
    std::string username;
    int         final_balance;
    int         net_change;   // final_balance − starting_balance
};

namespace serialise {

std::string error_code_str(WsErrorCode c) noexcept;
std::string side           (engine::Side s) noexcept;
std::string opt_price      (std::optional<int32_t> p) noexcept;

// Shared between on-connect snapshot and broadcast_book_update so the wire
// format stays in sync regardless of call site.
// best_bid_slot / best_ask_slot are null when no orders exist on that side.
std::string book_update_payload(const std::string&     suit,
                                 std::optional<int32_t> best_bid,
                                 std::optional<int32_t> best_ask,
                                 std::optional<int32_t> best_bid_slot = std::nullopt,
                                 std::optional<int32_t> best_ask_slot = std::nullopt);

std::string round_end_payload  (const engine::RoundResult& result,
                                 const std::vector<int>&    available_cash);
// AGENT-CTX: usernames is a slot-indexed vector (usernames[i] = username for slot i).
// Passed as plain strings rather than SlotInfo to avoid pulling game_session.h
// into this header (circular dependency risk). Slot index = position in vector.
std::string round_start_payload(int                             slot,
                                 const engine::PlayerHand&       hand,
                                 const std::string&              round_end_at,
                                 int                             effective_balance,
                                 const std::vector<std::string>& usernames,
                                 const std::vector<int>&         all_hand_totals,
                                 const std::vector<int>&         all_balances);

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

// ── GameSession outbound payloads (return std::string, no socket I/O) ────────
// AGENT-CTX: These functions return JSON strings rather than performing
// socket sends because GameSession runs on its own thread and has no WsHandle.
// WsServer drains the outbound queue and calls ws->send() with these strings.

// next_round_at: ISO timestamp, or empty string → serialised as JSON null.
std::string inter_round_payload(
    int                                round_number,
    const std::string&                 goal_suit,
    const std::vector<WirePlayerResult>& results,
    const std::string&                 next_round_at);

std::string game_ended_payload(
    const std::vector<WireRoundSummary>&  rounds,
    const std::vector<WireFinalStanding>& standings);

std::string game_player_left_payload(int player_slot, const std::string& username);

std::string session_error_payload(const std::string& message);

} // namespace serialise

} // namespace anjeer::server
