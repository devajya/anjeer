#include <catch2/catch_test_macros.hpp>

#include <thread>

#include "server/rate_limiter.h"

using namespace anjeer::server;
using namespace std::chrono_literals;

static RateLimitConfig cfg(double cap, double refill,
                            int32_t threshold = 100,
                            int32_t secs      = 60) {
    return { cap, refill, threshold, secs };
}

TEST_CASE("under capacity: all requests allowed", "[rate_limiter]") {
    RateLimiter rl(cfg(5.0, 0.0));
    for (int i = 0; i < 5; ++i)
        REQUIRE(rl.check(1) == RateLimitResult::Allow);
}

TEST_CASE("over capacity: next request returns Warn", "[rate_limiter]") {
    RateLimiter rl(cfg(5.0, 0.0));
    for (int i = 0; i < 5; ++i) rl.check(1);
    REQUIRE(rl.check(1) == RateLimitResult::Warn);
}

TEST_CASE("different player ids have independent buckets", "[rate_limiter]") {
    RateLimiter rl(cfg(2.0, 0.0));
    rl.check(1); rl.check(1);
    REQUIRE(rl.check(2) == RateLimitResult::Allow);
}

TEST_CASE("token bucket refills over time", "[rate_limiter]") {
    RateLimiter rl(cfg(3.0, 1000.0));  // 1000 tok/s — full refill in ~3 ms
    rl.check(1); rl.check(1); rl.check(1);
    REQUIRE(rl.check(1) == RateLimitResult::Warn);
    std::this_thread::sleep_for(5ms);
    REQUIRE(rl.check(1) == RateLimitResult::Allow);
}

TEST_CASE("violation streak triggers Suspend at threshold", "[rate_limiter]") {
    RateLimiter rl(cfg(1.0, 0.0, /*threshold=*/3));
    rl.check(1);                                          // Allow — consumes the token
    REQUIRE(rl.check(1) == RateLimitResult::Warn);        // streak=1
    REQUIRE(rl.check(1) == RateLimitResult::Warn);        // streak=2
    REQUIRE(rl.check(1) == RateLimitResult::Suspend);     // streak=3 → suspend
}

TEST_CASE("is_suspended true immediately after Suspend result", "[rate_limiter]") {
    RateLimiter rl(cfg(1.0, 0.0, /*threshold=*/1, /*secs=*/60));
    rl.check(1);  // Allow
    rl.check(1);  // Suspend (threshold=1)
    REQUIRE(rl.is_suspended(1));
}

TEST_CASE("further checks return Suspend while window active", "[rate_limiter]") {
    RateLimiter rl(cfg(1.0, 0.0, 1, 60));
    rl.check(1);
    rl.check(1);
    REQUIRE(rl.check(1) == RateLimitResult::Suspend);
    REQUIRE(rl.check(1) == RateLimitResult::Suspend);
}

TEST_CASE("is_suspended false after suspension window expires", "[rate_limiter]") {
    RateLimiter rl(cfg(1.0, 0.0, 1, /*secs=*/1));
    rl.check(1);
    rl.check(1);
    REQUIRE(rl.is_suspended(1));
    std::this_thread::sleep_for(1100ms);
    REQUIRE_FALSE(rl.is_suspended(1));
}

TEST_CASE("cleanup_expired removes expired entries", "[rate_limiter]") {
    RateLimiter rl(cfg(1.0, 0.0, 1, 1));
    rl.check(1); rl.check(1);  // suspend player 1
    rl.check(2); rl.check(2);  // suspend player 2
    std::this_thread::sleep_for(1100ms);
    rl.cleanup_expired();
    REQUIRE_FALSE(rl.is_suspended(1));
    REQUIRE_FALSE(rl.is_suspended(2));
}

TEST_CASE("cleanup_expired preserves active suspensions", "[rate_limiter]") {
    RateLimiter rl(cfg(1.0, 0.0, 1, 60));
    rl.check(1); rl.check(1);
    rl.cleanup_expired();
    REQUIRE(rl.is_suspended(1));
}

TEST_CASE("successful request resets violation streak", "[rate_limiter]") {
    // capacity=2, threshold=3: exhaust → 2 warns, refill → allow (streak reset)
    // then need 3 fresh violations before suspend, proving streak was cleared
    RateLimiter rl(cfg(2.0, 1000.0, /*threshold=*/3, 60));
    rl.check(1); rl.check(1);                         // Allow × 2 — exhaust
    REQUIRE(rl.check(1) == RateLimitResult::Warn);    // streak=1
    REQUIRE(rl.check(1) == RateLimitResult::Warn);    // streak=2
    std::this_thread::sleep_for(5ms);                 // refill to capacity (2 tokens)
    REQUIRE(rl.check(1) == RateLimitResult::Allow);   // consumes 1 — streak reset to 0
    rl.check(1);                                      // consumes last token (Allow)
    // streak=0; need 3 violations now (not 1) before suspend
    REQUIRE(rl.check(1) == RateLimitResult::Warn);    // streak=1 — proves reset worked
    REQUIRE(rl.check(1) == RateLimitResult::Warn);    // streak=2
    REQUIRE(rl.check(1) == RateLimitResult::Suspend); // streak=3 → suspend
}
