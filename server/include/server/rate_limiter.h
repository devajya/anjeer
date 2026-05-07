#pragma once

#include <chrono>
#include <cstdint>
#include <unordered_map>

namespace anjeer::server {

// Mirrors config.h RateLimitConfig; defined here so rate_limiter.h has no
// dependency on config.h and can be tested in isolation.
struct RateLimitConfig {
    double  capacity          = 20.0;
    double  refill_rate       = 5.0;   // tokens per second
    int32_t suspend_threshold = 50;    // consecutive violations before suspension
    int32_t suspend_seconds   = 30;
};

enum class RateLimitResult { Allow, Warn, Suspend };

// ARCHITECTURE-NOTE: All state is in-process. RateLimiter is intentionally
// NOT thread-safe — it is called exclusively from the uWS event-loop thread.
// The DB lookup on API key auth in the upgrade handler is also synchronous on
// this thread; both are accepted single-node risks flagged for Slice 16
// (Redis token bucket + async auth middleware).
class RateLimiter {
public:
    explicit RateLimiter(const RateLimitConfig& cfg);

    // Call once per inbound message for a connection.
    //   Allow   — token consumed, proceed normally
    //   Warn    — bucket empty, send rate_limit_warning but keep connection
    //   Suspend — consecutive violations hit threshold; caller must close the
    //             connection and reject reconnects via is_suspended()
    RateLimitResult check(int32_t player_id);

    // True if the player is within an active suspension window.
    // Call this on every WS upgrade before accepting the connection.
    bool is_suspended(int32_t player_id) const;

    // Remove entries whose suspension window has elapsed.
    // Call periodically (e.g. on the 60 s uWS cleanup timer).
    void cleanup_expired();

private:
    struct Bucket {
        double  tokens          = 0.0;
        std::chrono::steady_clock::time_point last_refill;
        int32_t violation_streak = 0;
    };

    Bucket& get_or_init(int32_t player_id);

    RateLimitConfig cfg_;
    std::unordered_map<int32_t, Bucket>                                 buckets_;
    std::unordered_map<int32_t, std::chrono::steady_clock::time_point>  suspended_until_;
};

} // namespace anjeer::server
