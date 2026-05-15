#include <catch2/catch_test_macros.hpp>

#include <regex>
#include <unordered_map>

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

// T7 — bot_uuid_for_slot returns a non-empty UUID4 after attach_to_session.
TEST_CASE("BotManager bot_uuid_for_slot returns UUID4 after attach", "[bots][manager][T7]") {
    BotScheduler sched(1);
    BotManager   mgr(sched, make_bots_cfg());

    auto [ok, uuid] = mgr.add_bot("lobby-t7a", BotDifficulty::Easy, 4);
    REQUIRE(ok);

    mgr.attach_to_session("lobby-t7a", {{uuid, 0}},
                          /*points_per_card=*/10, /*buy_in=*/100,
                          /*round_duration_s=*/60);

    const std::string result = mgr.bot_uuid_for_slot("lobby-t7a", 0);
    REQUIRE_FALSE(result.empty());

    // Validate UUID4: 8-4-4-4-12 hex, version nibble=4, variant byte in {8,9,a,b}.
    static const std::regex kUuid4{
        "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"
    };
    REQUIRE(std::regex_match(result, kUuid4));
}

// T7 — get_displaceable_bot_slot with two Easy bots returns the one with lower cash.
TEST_CASE("BotManager get_displaceable_bot_slot picks lower-cash Easy bot", "[bots][manager][T7]") {
    BotScheduler sched(1);
    BotManager   mgr(sched, make_bots_cfg());

    auto [ok1, u1] = mgr.add_bot("lobby-t7b", BotDifficulty::Easy, 4);
    auto [ok2, u2] = mgr.add_bot("lobby-t7b", BotDifficulty::Easy, 3);
    REQUIRE(ok1); REQUIRE(ok2);

    mgr.attach_to_session("lobby-t7b", {{u1, 0}, {u2, 1}},
                          10, 100, 60);

    // Slot 1 has less cash → it is more displaceable.
    const std::unordered_map<int,int> balances{{0, 200}, {1, 50}};
    REQUIRE(mgr.get_displaceable_bot_slot("lobby-t7b", balances) == 1);
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
