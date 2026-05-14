#include "server/bot_scheduler.h"

namespace anjeer::server {

BotScheduler::BotScheduler(int thread_count) {
    workers_.reserve(thread_count);
    for (int i = 0; i < thread_count; ++i)
        workers_.emplace_back([this]{ worker_loop(); });
}

BotScheduler::~BotScheduler() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
        // Clear heap so workers exit immediately after finishing any in-flight tick.
        heap_ = {};
    }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
}

BotHandle BotScheduler::register_bot(std::function<void()> tick_fn, int interval_ms) {
    BotHandle h = next_handle_.fetch_add(1, std::memory_order_relaxed);
    Entry e{h, std::move(tick_fn), interval_ms, clock::now()};
    {
        std::lock_guard<std::mutex> lk(mu_);
        heap_.push(std::move(e));
    }
    cv_.notify_one();
    return h;
}

void BotScheduler::deregister_bot(BotHandle handle) {
    std::unique_lock<std::mutex> lk(mu_);
    cancelled_.insert(handle);
    // Wait for any in-flight execution of this handle to complete.
    cv_.wait(lk, [this, handle]{ return running_.find(handle) == running_.end(); });
}

int BotScheduler::active_bot_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    // Count entries in heap that are not cancelled.
    // We can't iterate a priority_queue directly, so maintain a separate counter
    // if this becomes a hot path. For now, the size minus cancelled approximates it.
    return static_cast<int>(heap_.size()) - static_cast<int>(cancelled_.size());
}

void BotScheduler::worker_loop() {
    while (true) {
        std::unique_lock<std::mutex> lk(mu_);

        if (stop_ && heap_.empty()) break;

        if (heap_.empty()) {
            cv_.wait(lk, [this]{ return stop_ || !heap_.empty(); });
            if (stop_ && heap_.empty()) break;
            continue;
        }

        auto fire_time = heap_.top().next_fire;
        auto now = clock::now();

        if (fire_time > now) {
            cv_.wait_until(lk, fire_time);
            continue;
        }

        Entry entry = heap_.top();
        heap_.pop();

        if (cancelled_.count(entry.handle)) {
            cancelled_.erase(entry.handle);
            cv_.notify_all();  // wake any deregister_bot waiting on this handle
            continue;
        }

        running_.insert(entry.handle);
        lk.unlock();

        entry.fn();

        lk.lock();
        running_.erase(entry.handle);
        cv_.notify_all();  // wake deregister_bot waiters

        if (!cancelled_.count(entry.handle) && !stop_) {
            entry.next_fire = clock::now() + std::chrono::milliseconds(entry.interval_ms);
            heap_.push(std::move(entry));
            cv_.notify_one();
        }
    }
}

} // namespace anjeer::server
