#pragma once

#include "server/config.h"
#include "server/db.h"
#include "server/logger.h"
#include "server/session_queue.h"
#include "engine/engine.h"
#include <nlohmann/json.hpp>

#include <readerwriterqueue.h>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace anjeer::server {

enum class SessionPhase { Lobby, Countdown, RoundActive, InterRound, Ended };

// AGENT-CTX: SlotInfo represents one player seat for the lifetime of the session.
// connected tracks current WS state (can flip on each NetConnect/NetDisconnect).
// active=false means the player has permanently left — no further NetConnect will
// re-seat them. The distinction matters for vote-to-end threshold calculation.
struct SlotInfo {
    int64_t     player_id = -1;
    std::string username;
    int         balance   = 0;
    bool        connected = false;
    bool        active    = true;
};

class SessionRepo;  // forward declaration — used in persist helpers

// Non-owning bundle of stable server-wide dependencies passed to GameSession.
// All referenced objects must outlive the GameSession.
struct GameSessionContext {
    const ServerConfig& cfg;
    Logger&             server_log;
    Logger&             engine_log;
    std::mt19937        rng;
    DbPool&             db_pool;
};

class GameSession {
public:
    explicit GameSession(
        std::string                                       session_id,
        std::string                                       lobby_id,
        std::vector<SlotInfo>                             slots,
        GameSessionContext                                ctx,
        moodycamel::ReaderWriterQueue<NetEvent>&          inbound,
        moodycamel::ReaderWriterQueue<GameEvent>&         outbound);

    ~GameSession();
    GameSession(const GameSession&)            = delete;
    GameSession& operator=(const GameSession&) = delete;

    void start();     // launches game_loop_thread_; call once
    void shutdown();  // signals stop_, joins game_loop_thread_

    bool is_done() const { return done_.load(std::memory_order_acquire); }

private:
    // ── Game loop ─────────────────────────────────────────────────────────────
    void run();
    void tick();
    void process_inbound();

    // ── NetEvent handlers (called on game-loop thread only) ───────────────────
    void handle_connect    (const NetConnect&);
    void handle_disconnect (const NetDisconnect&);
    void handle_submit     (const NetSubmit&);
    void handle_nudge      (const NetNudge&);
    void handle_cancel         (const NetCancel&);
    void handle_start_game     ();
    void handle_vote_to_end    (int32_t slot);
    void handle_permanent_leave(int32_t slot);
    void handle_spectator_join (const NetSpectatorJoin&);
    void handle_spectator_leave(const NetSpectatorLeave&);
    void send_spectator_snapshot(int32_t spectator_id);

    // ── Session / round lifecycle ─────────────────────────────────────────────
    void begin_countdown  ();
    void begin_round      ();          // first round and every subsequent round
    void collect_buy_ins  ();
    void end_round        ();
    void begin_inter_round(const std::vector<struct WirePlayerResult>& results,
                            const std::string& goal_suit);
    void end_game         (bool forced);

    // AGENT-CTX: check_end_condition is called at InterRound transitions and on
    // disconnect. It does NOT fire during RoundActive because we allow temporary
    // disconnection without ending the round — the round timer drives expiry.
    void check_end_condition();
    int  majority_threshold() const { return (active_player_count_ + 1) / 2; }

    // ── Engine event pipeline ─────────────────────────────────────────────────
    // AGENT-CTX: Returns true if a trade occurred in the batch. Callers use
    // this to decide whether to apply global wipe. Mirrors the old GameSession's
    // dispatch_events but emits via outbound queue instead of direct ws->send().
    bool dispatch_events    (int32_t slot, const std::vector<engine::OrderEvent>&);
    void apply_global_wipe  ();
    void apply_card_transfers (const std::vector<engine::OrderEvent>&);
    void apply_trade_settlements(const std::vector<engine::OrderEvent>&);

    // ── Post-trade state pipeline ─────────────────────────────────────────────
    void apply_post_trade_state(const std::vector<engine::OrderEvent>&);

    // ── Delta table ───────────────────────────────────────────────────────────
    // AGENT-CTX: Broadcast as a full snapshot after each trade so clients never
    // accumulate increments and risk desync.
    void apply_trade_delta(int buyer_slot, int seller_slot, int suit_idx);
    void reset_delta_table();
    void broadcast_delta_update();

    // ── Outbound helpers ──────────────────────────────────────────────────────
    void emit_broadcast           (const std::string& json);
    void emit_targeted            (int32_t slot, const std::string& json);
    void emit_spectator_targeted  (int32_t spectator_id, const std::string& json);
    void emit_spectator_broadcast (const std::string& json);
    void emit_error               (int32_t slot, std::string_view code, std::string_view message);
    void broadcast_waiting_for_start();

    // ── DB writes (called at game end or on crash — never per-tick) ──────────
    void persist_game_result();
    void persist_error(const std::string& type, const std::string& msg,
                       const nlohmann::json& ctx = nlohmann::json::object());

    // ── ISO timestamp helpers ─────────────────────────────────────────────────
    static std::string to_iso_string(std::chrono::system_clock::time_point tp);
    static std::string steady_to_iso(std::chrono::steady_clock::time_point tp);

    // ── Dependencies ──────────────────────────────────────────────────────────
    const ServerConfig& cfg_;
    Logger&             server_log_;
    Logger&             engine_log_;
    std::mt19937&       rng_;
    DbPool&             db_pool_;

    moodycamel::ReaderWriterQueue<NetEvent>&  inbound_;
    moodycamel::ReaderWriterQueue<GameEvent>& outbound_;

    // ── Identity ──────────────────────────────────────────────────────────────
    std::string session_id_;
    std::string lobby_id_;

    // ── Player state ──────────────────────────────────────────────────────────
    std::vector<SlotInfo>    slots_;
    std::vector<bool>        vote_to_end_;
    std::vector<bool>        funded_this_round_;   // set by collect_buy_ins each round
    int                      active_player_count_ = 0;
    std::unordered_set<int32_t> spectator_ids_;   // IDs only — WsHandles stay in WsServer

    // AGENT-CTX: Grace window before ending on all-disconnected. 500ms lets
    // WsServer reconnect a client to a freed slot (slot reuse for test compat)
    // before we declare the session dead. Without this, a single-tick all-zero
    // count on primer-client disconnect/reconnect races would kill the session.
    bool                                       all_disconnected_      = false;
    std::chrono::steady_clock::time_point      all_disconnected_since_{};
    static constexpr std::chrono::milliseconds kReconnectGrace{500};

    // ── Round state ───────────────────────────────────────────────────────────
    SessionPhase phase_        = SessionPhase::Lobby;
    int          round_number_ = 0;
    std::string  current_round_id_;
    // AGENT-CTX: current_deck_ is the single authoritative source for all per-round
    // deck properties (goal_suit, bonus_pool, distribution). Points into the static
    // kDecks table — always valid after begin_round, null in Lobby/Countdown phase.
    const struct DeckDef* current_deck_ = nullptr;

    std::string current_goal_suit_str() const;  // convenience: suit_name(current_deck_->goal_suit)

    std::array<engine::OrderBook, 4>   books_;
    std::array<bool, 4>                active_suits_{};
    std::unique_ptr<engine::GameState> game_state_;

    // AGENT-CTX: delta_table_[player_slot][suit_index] — net cards gained this
    // round visible to all players. Suit indices match engine::kAllSuits order.
    std::array<std::array<int,4>, 4>   delta_table_{};

    // Per-round result accumulation for game_ended summary
    struct RoundSummary {
        int                              round_number;
        std::string                      goal_suit;
        std::vector<struct WirePlayerResult> results; // includes balance after payout
    };
    std::vector<RoundSummary> round_history_;

    // ── Timers (steady_clock — no wall-clock needed for interval checks) ──────
    std::chrono::steady_clock::time_point countdown_deadline_;
    std::chrono::steady_clock::time_point round_deadline_;
    std::chrono::steady_clock::time_point inter_round_deadline_;

    // ── Thread control ────────────────────────────────────────────────────────
    // AGENT-CTX: stop_ is written by shutdown() on the calling thread and read by
    // run() on the game-loop thread. memory_order_relaxed is sufficient for the
    // stop flag because no other data races through it — the join() in shutdown()
    // provides the happens-before synchronisation that makes all mutations visible.
    std::atomic<bool> stop_{false};
    std::atomic<bool> done_{false};
    std::thread       game_loop_thread_;
};

} // namespace anjeer::server
