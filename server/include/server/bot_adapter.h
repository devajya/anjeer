#pragma once

#include "server/bot_spawn_context.h"
#include "server/session_queue.h"
#include "engine/bots/bot_agent.h"

#include <readerwriterqueue.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>

namespace anjeer::server {

// Threading model — two SPSC queues, no locks:
//
//   uWS drain loop  ──→  event_queue_  ──→  BotScheduler thread
//   (on_game_event)       JSON strings        (tick: pop + decide)
//
//   BotScheduler    ──→  action_queue_  ──→  uWS drain loop
//   (tick: push)         NetEvents           (drain_to_session: pop → session inbound)
//
// The session's inbound ReaderWriterQueue stays SPSC — only the uWS thread
// enqueues to it, via drain_to_session().
class BotAdapter {
public:
    BotAdapter(
        std::unique_ptr<anjeer::engine::BotAgent> strategy,
        const BotSpawnContext& ctx,
        int sim_delay_ms,
        int tick_interval_ms,
        int tick_jitter_ms,
        int thinking_min_ms,
        int thinking_max_ms,
        int max_concurrent_orders,
        std::string log_path = ""
    );

    // ── uWS drain-loop thread ─────────────────────────────────────────────

    // Push JSON payload into event_queue_ (lock-free, no parsing).
    void on_game_event(std::string_view json_payload, bool is_targeted);

    // Pop from action_queue_ and push into the session's inbound queue.
    // Call from the same thread that owns the inbound queue.
    void drain_to_session(moodycamel::ReaderWriterQueue<NetEvent>& session_inbound);

    // ── BotScheduler tick thread ──────────────────────────────────────────

    // Drain event_queue_, update snapshot + strategy, call decide(),
    // push result to pending_. Drain expired pending_ into action_queue_.
    void tick();

    // ── Accessed from either thread (atomic) ─────────────────────────────

    int  slot()     const { return player_slot_; }
    bool is_alive() const { return alive_.load(std::memory_order_relaxed); }
    void teardown();

private:
    using clock      = std::chrono::steady_clock;
    using time_point = clock::time_point;
    using sys_tp     = std::chrono::system_clock::time_point;

    std::unique_ptr<anjeer::engine::BotAgent> strategy_;
    int     player_slot_;
    int     sim_delay_ms_;
    int     tick_interval_ms_;
    int     tick_jitter_ms_;
    int     thinking_min_ms_;
    int     thinking_max_ms_;
    int     max_concurrent_orders_;
    int32_t points_per_card_;
    int32_t buy_in_;
    int     round_duration_s_;

    std::atomic<bool> alive_{true};

    moodycamel::ReaderWriterQueue<std::string> event_queue_{256};
    moodycamel::ReaderWriterQueue<NetEvent>    action_queue_{64};

    // ── Accessed only from tick() ─────────────────────────────────────────
    anjeer::engine::BotGameSnapshot snapshot_;
    sys_tp round_end_time_;
    bool   round_end_valid_ = false;

    std::deque<std::pair<time_point, NetEvent>>  pending_;
    int                                          actions_in_flight_       = 0;
    int                                          next_decide_interval_ms_ = 0;
    time_point                                   last_decide_at_{};
    std::deque<time_point>                       flight_deadlines_;
    std::mt19937_64                              adapter_rng_;

    void     process_event(const std::string& json);
    NetEvent action_to_net_event(const anjeer::engine::BotAction& action);
    static sys_tp parse_iso(const std::string& iso);

    std::ofstream log_;
    void log_event(std::string_view label, std::string_view detail);
    void log_tick(const std::vector<anjeer::engine::BotAction>& actions);
    std::string bid_ask_str(int suit_idx) const;
    static std::string now_str();
};

} // namespace anjeer::server
