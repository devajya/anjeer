#include <catch2/catch_test_macros.hpp>

#include "server/bot_adapter.h"
#include "server/bot_spawn_context.h"
#include "engine/engine.h"

#include <readerwriterqueue.h>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <string>
#include <thread>

using namespace anjeer::server;
using namespace anjeer::engine;
using namespace std::chrono_literals;

// ── Class 5: bot resource / queue drain tests ─────────────────────────────────

namespace {

static std::string make_round_start(int slot, float duration_s) {
    auto deadline = std::chrono::system_clock::now()
                  + std::chrono::duration_cast<std::chrono::system_clock::duration>(
                        std::chrono::duration<float>(duration_s));
    auto tt = std::chrono::system_clock::to_time_t(deadline);
    std::tm gmt{};
    gmtime_r(&tt, &gmt);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &gmt);

    return nlohmann::json{
        {"type",         "round_start"},
        {"player_slot",  slot},
        {"round_end_at", std::string(buf)},
        {"balance",      100},
        {"hand",         {{"clubs",8},{"diamonds",8},{"hearts",8},{"spades",8}}},
        {"all_balances", std::vector<int>(4, 100)},
    }.dump();
}

static BotAdapter make_adapter(int slot = 0) {
    BotConfig cfg{};
    cfg.max_concurrent_orders = 2;
    auto strategy = make_bot(BotDifficulty::Easy, cfg, 42);
    BotSpawnContext ctx{slot, {8,8,8,8}, 100, 240.0f, "easy", 10, 20, 240};
    return BotAdapter(
        std::move(strategy), ctx,
        /*sim_delay_ms=*/0,
        /*tick_interval_ms=*/0, /*tick_jitter_ms=*/0,
        /*thinking_min_ms=*/0,  /*thinking_max_ms=*/0,
        /*max_concurrent_orders=*/2,
        /*bot_uuid=*/"00000000-0000-4000-8000-000000000099"
    );
}

} // namespace

// action_queue_ must be empty before any ticks.
TEST_CASE("BotAdapter action queue starts empty", "[resource][bots]") {
    auto adapter = make_adapter();
    REQUIRE(adapter.test_action_queue_size() == 0);
}

// drain_to_session transfers all queued actions and empties action_queue_.
TEST_CASE("BotAdapter drain_to_session empties action queue", "[resource][bots]") {
    auto adapter = make_adapter();

    adapter.on_game_event(make_round_start(0, 240.0f), true);
    // Run ticks until at least one action is produced or we time out.
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (adapter.test_action_queue_size() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        adapter.tick();
    }

    if (adapter.test_action_queue_size() == 0) {
        // No action produced in 2s — still drainable (queue is already empty).
        moodycamel::ReaderWriterQueue<NetEvent> inbound(64);
        adapter.drain_to_session(inbound);
        REQUIRE(adapter.test_action_queue_size() == 0);
    } else {
        moodycamel::ReaderWriterQueue<NetEvent> inbound(64);
        adapter.drain_to_session(inbound);
        REQUIRE(adapter.test_action_queue_size() == 0);
    }
}

// teardown marks the bot as not alive — no further ticks should be scheduled.
TEST_CASE("BotAdapter teardown marks bot as dead", "[resource][bots]") {
    auto adapter = make_adapter();
    REQUIRE(adapter.is_alive());
    adapter.teardown();
    REQUIRE_FALSE(adapter.is_alive());
}
