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

    CHECK(cfg.host                  == "127.0.0.1");
    CHECK(cfg.port                  == 9999);
    CHECK(cfg.heartbeat_interval_ms == 500);
    CHECK(cfg.ping_interval_ms      == 500);
    CHECK(cfg.ping_timeout_ms       == 1500);
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
    CHECK_THROWS_AS(
        load_config(std::string(TEST_FIXTURES_DIR) + "/missing_field.json"),
        std::runtime_error
    );
}
