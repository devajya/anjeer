#include <catch2/catch_test_macros.hpp>
#include <readerwriterqueue.h>
#include <atomic>
#include <thread>

// AGENT-CTX: This file only tests the queue contract and data-race freedom.
// It is registered as RUN_SERIAL to prevent interference from other test
// processes that also spin threads, which can produce spurious TSAN reports
// when multiple ThreadSanitizer shadow stacks collide in the same process group.
//
// To run with full sanitizer coverage:
//   cmake --preset tsan && make test-unit
//
// The test will pass in a normal build (no TSAN) and serve as a functional
// regression guard; races are only reported when compiled with -fsanitize=thread.

TEST_CASE("T1: SPSC queue has no data race under concurrent producer/consumer",
          "[tsan][queue]") {
    // AGENT-CTX: Queue capacity hint set to N so the internal ring never needs
    // to grow. ReaderWriterQueue grows automatically when full, but growing
    // involves an allocation visible to TSAN — pre-sizing avoids that noise.
    constexpr int N = 10'000;
    moodycamel::ReaderWriterQueue<int> q(N);

    std::atomic<int> consumed{0};

    std::thread producer([&] {
        for (int i = 0; i < N; ++i) {
            // AGENT-CTX: Spin until enqueue succeeds. try_enqueue fails only
            // when the queue is full (shouldn't happen with capacity=N, but
            // the spin is safe in case of any edge case).
            while (!q.try_enqueue(i)) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&] {
        int val = 0;
        int count = 0;
        while (count < N) {
            if (q.try_dequeue(val)) {
                ++count;
            } else {
                std::this_thread::yield();
            }
        }
        consumed.store(count, std::memory_order_relaxed);
    });

    producer.join();
    consumer.join();

    REQUIRE(consumed.load(std::memory_order_relaxed) == N);
}
