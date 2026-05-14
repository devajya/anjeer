#pragma once

// AGENT-CTX: SimpleCache is a per-process in-memory TTL cache. It is intentionally
// simple: one mutex, one unordered_map, time measured via steady_clock.
// At deploy time this is expected to be replaced by a Redis-backed layer
// (see slice_definitions.txt). Do not add persistence, LRU eviction, or
// distributed invalidation here — those belong in the Redis layer.
//
// Thread safety: all public methods are safe to call from multiple threads.

#include <chrono>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace anjeer::server {

template<typename K, typename V>
class SimpleCache {
public:
    explicit SimpleCache(std::chrono::seconds ttl) : ttl_(ttl) {}

    std::optional<V> get(const K& key) const {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lk(mu_);
        auto it = store_.find(key);
        if (it == store_.end() || now >= it->second.expires_at)
            return std::nullopt;
        return it->second.value;
    }

    void set(const K& key, V value) {
        const auto exp = std::chrono::steady_clock::now() + ttl_;
        std::lock_guard<std::mutex> lk(mu_);
        store_[key] = { std::move(value), exp };
    }

    void invalidate(const K& key) {
        std::lock_guard<std::mutex> lk(mu_);
        store_.erase(key);
    }

    // Removes all expired entries. Call periodically to prevent unbounded growth
    // on long-running servers with many unique keys.
    void cleanup_expired() {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = store_.begin(); it != store_.end(); ) {
            if (now >= it->second.expires_at)
                it = store_.erase(it);
            else
                ++it;
        }
    }

private:
    struct Entry {
        V                                        value;
        std::chrono::steady_clock::time_point    expires_at;
    };

    mutable std::mutex            mu_;
    std::unordered_map<K, Entry>  store_;
    std::chrono::seconds          ttl_;
};

} // namespace anjeer::server
