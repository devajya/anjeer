#pragma once

#include "server/config.h"
#include "server/logger.h"
#include "engine/engine.h"

// AGENT-CTX: App.h is the uWS combined header. GameSession holds WsHandle
// (uWS::WebSocket*) in its connection set and player_slots, so the uWS types
// must be visible here. This coupling is intentional until Slice 6 when
// GameSession moves behind a thread boundary and WsHandle is replaced by a
// slot-id + broadcast callback abstraction (BroadcastFn).
#include <App.h>
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string_view>
#include <thread>
#include <vector>

namespace anjeer::server {

// AGENT-CTX: Exposed here so future HTTP endpoints (/api/status) and
// LobbyManager (Slice 6) can inspect phase without depending on game logic.
enum class RoundPhase { Waiting, Active, Scoring, Ended };

// AGENT-CTX: PerSocketData is uWS per-connection user data.
// player_slot is 0-indexed; -1 = unassigned sentinel.
// Slice 5 adds optional<int64_t> player_id (DB identity) here but does not
// enforce it on the WS path — Slice 6 wires WS auth via lobby join token.
struct PerSocketData {
    int32_t player_slot = -1;
    // AGENT-CTX: player_id populated in Slice 6 when lobby-join validates JWT.
    // Left as a comment placeholder so the field position is reserved.
};

using WsHandle = uWS::WebSocket<false, true, PerSocketData>*;

// AGENT-CTX: Complete wire-protocol error code set for the WS channel.
// Engine codes reach here via to_ws_error_code() in game_session.cpp;
// server-layer codes (UnknownSuit, MalformedMessage, etc.) are referenced
// directly at each call site. Engine never knows about suits or JSON.
enum class WsErrorCode {
    PriceOutOfRange,
    OrderNotFound,
    NotYourOrder,
    UnknownSuit,
    MalformedMessage,
    ServerFull,
    RoundNotActive,
    InsufficientBalance,
};

// ─────────────────────────────────────────────────────────────────────────────
// GameSession
//
// AGENT-CTX: Owns all mutable game state for one game instance. In Slice 1-5,
// WsServer creates exactly one GameSession. In Slice 6, WsServer holds a
// map<lobby_id, GameSession> — adding a lobby is a one-line type change here.
//
// All methods run on the uWS event-loop thread (single-threaded access).
// countdown_thread_ and round_timer_thread_ only read game state via
// loop_->defer(), which marshals the actual mutation back to the event loop.
// Do not call any method from outside the event loop without wrapping in defer.
// ─────────────────────────────────────────────────────────────────────────────
class GameSession {
public:
    // AGENT-CTX: Constructor does NOT take ownership of cfg, loggers, loop, rng.
    // All are owned by WsServer::run() and outlive GameSession — safe because
    // run() joins all threads via shutdown() before returning.
    explicit GameSession(
        std::array<engine::OrderBook, 4> books,
        std::array<bool, 4>              active_suits,
        const ServerConfig&              cfg,
        Logger&                          server_log,
        Logger&                          engine_log,
        uWS::Loop*                       loop,
        std::mt19937&                    rng);

    ~GameSession();

    // Non-copyable, non-movable: holds references and atomic members.
    GameSession(const GameSession&)            = delete;
    GameSession& operator=(const GameSession&) = delete;
    GameSession(GameSession&&)                 = delete;
    GameSession& operator=(GameSession&&)      = delete;

    // ── Public interface — called from WsServer's uWS callbacks ──────────────

    void on_connect(WsHandle ws);
    void on_message(WsHandle ws, std::string_view msg, uWS::OpCode op);
    void on_close  (WsHandle ws, int code, std::string_view reason);

    // AGENT-CTX: Call shutdown() before GameSession goes out of scope.
    // Sets running_=false and joins countdown_thread_ and round_timer_thread_.
    // Without this, threads that captured `this` would access destroyed memory.
    void shutdown();

private:
    // ── Non-owning runtime dependencies ──────────────────────────────────────
    const ServerConfig& cfg_;
    Logger&             server_log_;
    Logger&             engine_log_;
    uWS::Loop*          loop_;
    std::mt19937&       rng_;

    // ── Game state (owned) ───────────────────────────────────────────────────
    std::array<engine::OrderBook, 4>   books_;
    std::array<bool, 4>                active_suits_{};
    std::set<WsHandle>                 connections_;
    std::vector<WsHandle>              player_slots_;
    std::vector<int>                   available_cash_;
    int                                connected_count_       = 0;
    bool                               countdown_in_progress_ = false;
    bool                               round_started_         = false;
    RoundPhase                         round_phase_           = RoundPhase::Waiting;
    std::unique_ptr<engine::GameState> game_state_;

    // ── Thread management ────────────────────────────────────────────────────
    // AGENT-CTX: running_ is read by timer threads; written by shutdown() on
    // the event-loop thread. std::atomic ensures the write is visible across
    // threads without a lock. No other fields are accessed cross-thread —
    // all game-state mutations are deferred back to the event loop via loop_->defer().
    std::atomic<bool>          running_{true};
    std::optional<std::thread> countdown_thread_;
    std::optional<std::thread> round_timer_thread_;

    // ── Message handlers ─────────────────────────────────────────────────────
    void handle_submit    (WsHandle ws, const nlohmann::json& j, int32_t player_slot);
    void handle_nudge     (WsHandle ws, const nlohmann::json& j, int32_t player_slot);
    void handle_cancel    (WsHandle ws, const nlohmann::json& j, int32_t player_slot);
    // AGENT-CTX: handle_start_game replaces the N-connections auto-trigger (Slice 5 Task 2).
    // Any connected player can call this in Slice 5. Slice 6 adds lobby-owner enforcement.
    void handle_start_game();

    // ── Round lifecycle ──────────────────────────────────────────────────────
    // AGENT-CTX: broadcast_waiting_for_start is called on every connect/disconnect
    // while phase == Waiting. Replaces the implicit auto-start side-effect.
    void broadcast_waiting_for_start();
    void begin_countdown();
    void handle_deal_and_start();
    void handle_round_expiry();

    // ── Event processing pipeline ────────────────────────────────────────────
    bool dispatch_events       (WsHandle ws, const std::vector<engine::OrderEvent>& events);
    void apply_global_wipe     ();
    void apply_card_transfers  (const std::vector<engine::OrderEvent>& events);
    void apply_trade_settlements(const std::vector<engine::OrderEvent>& events);
    void apply_engine_events   (WsHandle ws, const std::vector<engine::OrderEvent>& events);
};

} // namespace anjeer::server
