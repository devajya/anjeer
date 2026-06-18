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

// Destructor must flush and close the file handle — subsequent open must succeed.
TEST_CASE("Logger destructor releases file handle", "[logger]") {
    ::unsetenv("ANJEER_LOG_FILE");
    const std::string path = "/tmp/anjeer_test_logger_dtor.log";
    std::filesystem::remove(path);
    {
        Logger lg(path);
        lg.info("test", "entry");
    } // destructor runs here
    // After destruction, the file must be openable (handle released).
    std::ifstream f(path);
    REQUIRE(f.good());
}

// none-mode: no bytes written to the log path even after multiple log calls.
TEST_CASE("Logger none-mode writes zero bytes to log path", "[logger][none]") {
    ::setenv("ANJEER_LOG_FILE", "none", 1);
    const std::string path = "/tmp/anjeer_test_none_bytes.log";
    std::filesystem::remove(path);
    {
        Logger lg(path);
        for (int i = 0; i < 10; ++i) lg.info("test", "msg");
    }
    REQUIRE_FALSE(std::filesystem::exists(path));
    ::unsetenv("ANJEER_LOG_FILE");
}
