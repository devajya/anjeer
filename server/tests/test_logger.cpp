#include <catch2/catch_test_macros.hpp>
#include "server/logger.h"
#include <cstdlib>
#include <filesystem>

using anjeer::server::Logger;

TEST_CASE("Logger none-mode skips file creation", "[logger][none]") {
    ::setenv("ANJEER_LOG_FILE", "none", 1);
    const std::string path = "/tmp/anjeer_test_should_not_exist.log";
    std::filesystem::remove(path);

    REQUIRE_NOTHROW(Logger(path));
    REQUIRE_FALSE(std::filesystem::exists(path));

    ::unsetenv("ANJEER_LOG_FILE");
}

TEST_CASE("Logger none-mode log() writes to stdout without crashing", "[logger][none]") {
    ::setenv("ANJEER_LOG_FILE", "none", 1);
    Logger lg("/tmp/anjeer_test_noop.log");
    REQUIRE_NOTHROW(lg.info("test", "message"));
    ::unsetenv("ANJEER_LOG_FILE");
}

TEST_CASE("Logger normal mode creates file", "[logger]") {
    ::unsetenv("ANJEER_LOG_FILE");
    const std::string path = "/tmp/anjeer_test_logger_normal.log";
    std::filesystem::remove(path);
    REQUIRE_NOTHROW(Logger(path));
    REQUIRE(std::filesystem::exists(path));
}
