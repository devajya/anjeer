#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_set>
#include <vector>

namespace anjeer::server {

using BotHandle = uint32_t;

class BotScheduler {
public:
    explicit BotScheduler(int thread_count);
    ~BotScheduler();

    // Register a recurring callback. Returns an opaque handle.
    BotHandle register_bot(std::function<void()> tick_fn, int interval_ms);

    // Deregister and wait for any in-flight tick to finish.
    void deregister_bot(BotHandle handle);

    int thread_count()    const { return static_cast<int>(workers_.size()); }
    int active_bot_count() const;

    BotScheduler(const BotScheduler&)            = delete;
    BotScheduler& operator=(const BotScheduler&) = delete;

private:
    using clock      = std::chrono::steady_clock;
    using time_point = clock::time_point;

    struct Entry {
        BotHandle             handle;
        std::function<void()> fn;
        int                   interval_ms;
        time_point            next_fire;

        bool operator>(const Entry& o) const { return next_fire > o.next_fire; }
    };

    std::vector<std::thread> workers_;
    mutable std::mutex       mu_;
    std::condition_variable  cv_;
    bool                     stop_ = false;

    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap_;
    std::unordered_set<BotHandle> cancelled_;
    std::unordered_set<BotHandle> running_;
    std::atomic<BotHandle>        next_handle_{1};

    void worker_loop();
};

} // namespace anjeer::server
