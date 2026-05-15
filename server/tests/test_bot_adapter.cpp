#include <catch2/catch_test_macros.hpp>

#include "server/bot_adapter.h"
#include "engine/engine.h"

#include <nlohmann/json.hpp>
#include <readerwriterqueue.h>

#include <chrono>
#include <thread>

using namespace anjeer::server;
using namespace anjeer::engine;
using namespace std::chrono_literals;

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string make_round_start_json(int slot, std::array<int,4> hand,
                                         float duration_s, int player_count) {
    // Build a round_end_at timestamp 'duration_s' seconds from now.
    auto deadline = std::chrono::system_clock::now()
                  + std::chrono::duration_cast<std::chrono::system_clock::duration>(
                        std::chrono::duration<float>(duration_s));
    auto tt = std::chrono::system_clock::to_time_t(deadline);
    std::tm gmt{};
    gmtime_r(&tt, &gmt);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &gmt);

    std::vector<int> balances(player_count, 100);
    return nlohmann::json{
        {"type",         "round_start"},
        {"player_slot",  slot},
        {"round_end_at", std::string(buf)},
        {"balance",      100},
        {"hand", {
            {"clubs",    hand[0]},
            {"diamonds", hand[1]},
            {"hearts",   hand[2]},
            {"spades",   hand[3]},
        }},
        {"all_balances", balances},
    }.dump();
}

static BotConfig make_test_bot_cfg() {
    BotConfig cfg;
    cfg.confidence_discount   = 0.80f;
    cfg.taker_threshold       = 0.85f;
    cfg.min_bid_ev            = 3.0f;
    cfg.max_ask_ev            = 7.0f;
    cfg.hand_size_cap         = 4;
    cfg.offload_threshold     = 3;
    cfg.max_concurrent_orders = 2;
    cfg.conviction_threshold  = 0.0f;
    cfg.max_resting_ms        = 6000;
    cfg.nudge_probability     = 0.0f;
    cfg.nudge_patience_ms     = 3000;
    cfg.nudge_max_gap         = 3;
    cfg.endgame_threshold_s   = 0;
    cfg.early_seed_threshold  = 0.0f;
    return cfg;
}

// T16 — BotAdapter does not enqueue an action until sim_delay_ms has elapsed.
TEST_CASE("BotAdapter applies simulated network delay before enqueue") {
    constexpr int delay_ms = 100;

    auto strategy = make_bot(BotDifficulty::Easy, make_test_bot_cfg(), 42);
    BotAdapter adapter(std::move(strategy),
                       BotSpawnContext{0, {}, 0, 0.0f, "easy", 10, 20, 240},
                       delay_ms,
                       /*tick_interval_ms=*/0, /*tick_jitter_ms=*/0,
                       /*thinking_min_ms=*/0,  /*thinking_max_ms=*/0,
                       /*max_concurrent_orders=*/2,
                       /*bot_uuid=*/"00000000-0000-4000-8000-000000000001");

    const std::string round_json = make_round_start_json(0, {10,10,10,10}, 240.0f, 4);
    adapter.on_game_event(round_json, true);

    // First tick: event is processed, action pushed to pending_ with
    // fire_time = now + 100 ms. The action is NOT yet in action_queue_.
    adapter.tick();

    moodycamel::ReaderWriterQueue<NetEvent> session_q(64);
    adapter.drain_to_session(session_q);

    NetEvent ev;
    REQUIRE_FALSE(session_q.try_dequeue(ev));  // delay not elapsed yet

    std::this_thread::sleep_for(110ms);

    // Second tick: pending_ item has expired; it moves to action_queue_.
    adapter.tick();
    adapter.drain_to_session(session_q);

    REQUIRE(session_q.try_dequeue(ev));  // action arrived after delay
    // Bot should have submitted an order (not a cancel or connect).
    REQUIRE(std::holds_alternative<NetSubmit>(ev));
}

// Extra: teardown prevents further actions from reaching the session queue.
TEST_CASE("BotAdapter teardown suppresses further action enqueues") {
    auto strategy = make_bot(BotDifficulty::Easy, make_test_bot_cfg(), 7);
    BotAdapter adapter(std::move(strategy),
                       BotSpawnContext{0, {}, 0, 0.0f, "easy", 10, 20, 240},
                       /*sim_delay_ms=*/0,
                       /*tick_interval_ms=*/0, /*tick_jitter_ms=*/0,
                       /*thinking_min_ms=*/0,  /*thinking_max_ms=*/0,
                       /*max_concurrent_orders=*/2,
                       /*bot_uuid=*/"00000000-0000-4000-8000-000000000002");

    const std::string round_json = make_round_start_json(0, {10,10,10,10}, 240.0f, 4);
    adapter.on_game_event(round_json, true);
    adapter.teardown();
    adapter.tick();  // alive_ is false; should produce nothing

    moodycamel::ReaderWriterQueue<NetEvent> session_q(64);
    adapter.drain_to_session(session_q);

    NetEvent ev;
    CHECK_FALSE(session_q.try_dequeue(ev));
}
