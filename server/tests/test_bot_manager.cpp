#include <catch2/catch_test_macros.hpp>

#include "server/bot_manager.h"
#include "server/bot_scheduler.h"
#include "server/config.h"

using namespace anjeer::server;
using namespace anjeer::engine;

static ServerConfig::BotsConfig make_bots_cfg() {
    ServerConfig::BotsConfig cfg;
    cfg.scheduler_threads    = 1;
    cfg.scheduler_tick_ms    = 100;
    cfg.sim_network_delay_ms = 0;
    return cfg;
}

// T13 — add_bot inserts bot in-memory and returns a valid uuid.
TEST_CASE("BotManager add_bot returns bot_uuid and is visible in get_bots", "[bots][manager]") {
    BotScheduler sched(1);
    BotManager   mgr(sched, make_bots_cfg());

    auto [ok, result] = mgr.add_bot("lobby-t13", BotDifficulty::Easy, /*open_slots=*/4);
    REQUIRE(ok);
    REQUIRE(!result.empty());

    const auto bots = mgr.get_bots("lobby-t13");
    REQUIRE(bots.size() == 1);
    REQUIRE(bots[0].bot_uuid  == result);
    REQUIRE(bots[0].difficulty == "easy");
    REQUIRE(bots[0].slot      == -1);  // unassigned until attach_to_session
}

// T14 — remove_bot removes the bot entry from in-memory state.
TEST_CASE("BotManager remove_bot removes bot from lobby", "[bots][manager]") {
    BotScheduler sched(1);
    BotManager   mgr(sched, make_bots_cfg());

    auto [ok, uuid] = mgr.add_bot("lobby-t14", BotDifficulty::Medium, 4);
    REQUIRE(ok);
    REQUIRE(mgr.remove_bot("lobby-t14", uuid));
    REQUIRE(mgr.get_bots("lobby-t14").empty());
    REQUIRE_FALSE(mgr.has_bots("lobby-t14"));
}

// T15 — add_bot returns an error when open_slots == 0.
TEST_CASE("BotManager add_bot rejects when no open slots remain", "[bots][manager]") {
    BotScheduler sched(1);
    BotManager   mgr(sched, make_bots_cfg());

    auto [ok, msg] = mgr.add_bot("lobby-t15", BotDifficulty::Hard, /*open_slots=*/0);
    REQUIRE_FALSE(ok);
    REQUIRE(!msg.empty());
}

// Extra — remove_bot returns false for an unknown uuid.
TEST_CASE("BotManager remove_bot returns false for unknown uuid", "[bots][manager]") {
    BotScheduler sched(1);
    BotManager   mgr(sched, make_bots_cfg());

    REQUIRE_FALSE(mgr.remove_bot("lobby-x", "no-such-uuid"));
}

// Extra — multiple bots can be added up to open_slots.
TEST_CASE("BotManager allows multiple bots in one lobby", "[bots][manager]") {
    BotScheduler sched(1);
    BotManager   mgr(sched, make_bots_cfg());

    auto [ok1, u1] = mgr.add_bot("lobby-multi", BotDifficulty::Easy,   4);
    auto [ok2, u2] = mgr.add_bot("lobby-multi", BotDifficulty::Medium, 3);
    auto [ok3, u3] = mgr.add_bot("lobby-multi", BotDifficulty::Hard,   2);
    REQUIRE(ok1); REQUIRE(ok2); REQUIRE(ok3);

    const auto bots = mgr.get_bots("lobby-multi");
    REQUIRE(bots.size() == 3);
    REQUIRE(bots[0].difficulty == "easy");
    REQUIRE(bots[1].difficulty == "medium");
    REQUIRE(bots[2].difficulty == "hard");
}
