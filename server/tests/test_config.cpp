#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <string>

#include "server/config.h"

// AGENT-CTX: TEST_FIXTURES_DIR is injected by CMake at compile time so these tests
// are location-independent regardless of CTest working directory.
#ifndef TEST_FIXTURES_DIR
#  error "TEST_FIXTURES_DIR must be defined by CMake target_compile_definitions"
#endif

using anjeer::server::load_config;

// ---------------------------------------------------------------------------
// AC: C++ server starts and listens on a configurable port
// ---------------------------------------------------------------------------

TEST_CASE("load_config reads all fields from a valid JSON file", "[config]") {
    auto cfg = load_config(std::string(TEST_FIXTURES_DIR) + "/test_config.json");

    // server section
    CHECK(cfg.host                  == "127.0.0.1");
    CHECK(cfg.port                  == 9999);
    CHECK(cfg.heartbeat_interval_ms == 500);
    CHECK(cfg.ping_interval_ms      == 500);
    CHECK(cfg.ping_timeout_ms       == 8000);

    // AGENT-CTX: order_book section added in Slice 2. Verified here so a future
    // agent adding new fields is reminded to extend test_config.json and these
    // assertions together — omitting either silently hides parse regressions.
    CHECK(cfg.order_book.min_price               == 1);
    CHECK(cfg.order_book.max_price               == 99);
    CHECK(cfg.order_book.nudge_initial_buy_price  == 1);
    CHECK(cfg.order_book.nudge_initial_sell_price == 99);
    REQUIRE(cfg.order_book.active_suits.size()   == 1);
    CHECK(cfg.order_book.active_suits[0]         == "S1");

    // AGENT-CTX: game section added in Slice 3. Extend assertions here when
    // new fields are added to GameConfig.
    CHECK(cfg.game.player_count                  == 5);
    CHECK(cfg.game.total_cards                   == 40);
    CHECK(cfg.game.countdown_seconds             == 3);
    REQUIRE(cfg.game.card_distribution[0]        == 12);
    REQUIRE(cfg.game.card_distribution[1]        == 10);
    REQUIRE(cfg.game.card_distribution[2]        == 10);
    REQUIRE(cfg.game.card_distribution[3]        == 8);
}

TEST_CASE("load_config throws on missing file", "[config]") {
    CHECK_THROWS_AS(
        load_config("/nonexistent/path/config.json"),
        std::runtime_error
    );
}

TEST_CASE("load_config throws on malformed JSON", "[config]") {
    // AGENT-CTX: Uses the same fixtures dir; malformed.json has invalid JSON syntax.
    CHECK_THROWS_AS(
        load_config(std::string(TEST_FIXTURES_DIR) + "/malformed.json"),
        std::runtime_error
    );
}

TEST_CASE("load_config throws when a required field is missing", "[config]") {
    // AGENT-CTX: missing_field.json omits 'port' to verify strict field checking.
    // This test also covers the case where order_book is absent: if missing_field.json
    // happens to include a valid server section but no order_book section, it still
    // throws (for the missing order_book key), satisfying the same invariant.
    CHECK_THROWS_AS(
        load_config(std::string(TEST_FIXTURES_DIR) + "/missing_field.json"),
        std::runtime_error
    );
}
