#include "server/rate_limiter.h"

#include <algorithm>

namespace anjeer::server {

using clock    = std::chrono::steady_clock;
using dseconds = std::chrono::duration<double>;

RateLimiter::RateLimiter(const RateLimitConfig& cfg) : cfg_(cfg) {}

RateLimiter::Bucket& RateLimiter::get_or_init(int32_t player_id) {
    auto [it, inserted] = buckets_.try_emplace(player_id);
    if (inserted) {
        it->second.tokens      = cfg_.capacity;
        it->second.last_refill = clock::now();
    }
    return it->second;
}

RateLimitResult RateLimiter::check(int32_t player_id) {
    if (is_suspended(player_id)) return RateLimitResult::Suspend;

    auto& b   = get_or_init(player_id);
    auto  now = clock::now();

    double elapsed = dseconds(now - b.last_refill).count();
    b.tokens       = std::min(cfg_.capacity, b.tokens + elapsed * cfg_.refill_rate);
    b.last_refill  = now;

    if (b.tokens >= 1.0) {
        b.tokens -= 1.0;
        b.violation_streak = 0;
        return RateLimitResult::Allow;
    }

    if (++b.violation_streak >= cfg_.suspend_threshold) {
        suspended_until_[player_id] = now + std::chrono::seconds(cfg_.suspend_seconds);
        b.violation_streak          = 0;
        return RateLimitResult::Suspend;
    }

    return RateLimitResult::Warn;
}

bool RateLimiter::is_suspended(int32_t player_id) const {
    auto it = suspended_until_.find(player_id);
    return it != suspended_until_.end() && clock::now() < it->second;
}

void RateLimiter::cleanup_expired() {
    auto now = clock::now();
    for (auto it = suspended_until_.begin(); it != suspended_until_.end(); ) {
        it = (now >= it->second) ? suspended_until_.erase(it) : std::next(it);
    }
}

} // namespace anjeer::server
