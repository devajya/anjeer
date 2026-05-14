#include <catch2/catch_test_macros.hpp>

#include "server/bot_scheduler.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace anjeer::server;
using namespace std::chrono_literals;

// T11 — Thread count is bounded by the configured ceiling even under load.
TEST_CASE("BotScheduler thread count stays at configured ceiling") {
    BotScheduler sched(4);

    std::atomic<int> concurrent_peak{0};
    std::atomic<int> current{0};

    for (int i = 0; i < 20; ++i) {
        sched.register_bot([&]() {
            int c = ++current;
            int expected = concurrent_peak.load();
            while (c > expected && !concurrent_peak.compare_exchange_weak(expected, c))
                ;
            std::this_thread::sleep_for(10ms);
            --current;
        }, 50);
    }

    std::this_thread::sleep_for(500ms);

    CHECK(sched.thread_count() == 4);
    CHECK(concurrent_peak.load() <= 4);
}

// T12 — Callback fires at approximately the registered interval.
TEST_CASE("BotScheduler fires callback at registered interval") {
    BotScheduler sched(2);

    std::atomic<int> count{0};
    sched.register_bot([&]{ ++count; }, 100);

    std::this_thread::sleep_for(550ms);

    // ~5 fires in 550 ms at a 100 ms interval; allow ±2 for scheduling jitter.
    CHECK(count.load() >= 4);
    CHECK(count.load() <= 7);
}

// T11b — deregister_bot returns only after an in-flight tick completes.
TEST_CASE("BotScheduler deregister_bot waits for in-flight tick") {
    BotScheduler sched(2);

    std::atomic<bool> tick_started{false};
    std::atomic<bool> tick_done{false};

    auto h = sched.register_bot([&]{
        tick_started = true;
        std::this_thread::sleep_for(80ms);
        tick_done = true;
    }, 50);

    // Wait until the tick is running.
    while (!tick_started) std::this_thread::sleep_for(5ms);

    sched.deregister_bot(h);

    // deregister_bot must not return before the tick finishes.
    CHECK(tick_done.load());
}
