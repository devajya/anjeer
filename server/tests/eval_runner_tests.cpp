#include <catch2/catch_test_macros.hpp>

#include "server/eval/eval_runner.h"
#include "server/eval/eval_module.h"
#include "engine/game_snapshot.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace anjeer::server::eval;
using namespace anjeer::engine;

namespace {

// Records which on_* methods were called, in order.
struct RecordingModule : EvalModule {
    enum class EventKind { RoundStart, Trade, Book, RoundEnd };

    std::mutex              mu;
    std::vector<EventKind>  events;
    std::vector<int32_t>    trade_prices;  // for FIFO-order verification

    void on_round_start(const GameStateSnapshot&) override {
        std::lock_guard<std::mutex> lk(mu);
        events.push_back(EventKind::RoundStart);
    }
    void on_trade_event(const EvalTradeEvent& ev) override {
        std::lock_guard<std::mutex> lk(mu);
        events.push_back(EventKind::Trade);
        trade_prices.push_back(ev.price);
    }
    void on_book_update(const EvalBookUpdate&) override {
        std::lock_guard<std::mutex> lk(mu);
        events.push_back(EventKind::Book);
    }
    void on_round_end(const GameStateSnapshot&) override {
        std::lock_guard<std::mutex> lk(mu);
        events.push_back(EventKind::RoundEnd);
    }

    size_t count() {
        std::lock_guard<std::mutex> lk(mu);
        return events.size();
    }
};

// Blocks in on_trade_event until released — used to stall the worker thread.
struct BlockingModule : EvalModule {
    std::atomic<bool> blocked{true};

    void on_round_start(const GameStateSnapshot&) override {}
    void on_trade_event(const EvalTradeEvent&) override {
        while (blocked.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    void on_book_update(const EvalBookUpdate&) override {}
    void on_round_end(const GameStateSnapshot&) override {}
};

EvalTradeEvent make_trade(int32_t price) {
    return {0, 1, price, Suit::Spades, 0};
}

void wait_for(RecordingModule& m, size_t n, std::chrono::milliseconds timeout = std::chrono::milliseconds(500)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (m.count() < n && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
}

} // namespace

// T1: EvalRunner starts and stops cleanly with no modules.
TEST_CASE("EvalRunner start/stop") {
    EvalRunner runner({}, nullptr);
    runner.start();
    runner.stop();
    // No crash, no hang → pass.
}

// T2: Events fanned out to all registered modules.
TEST_CASE("EvalRunner fans out round_start to all modules") {
    auto* a = new RecordingModule;
    auto* b = new RecordingModule;

    std::vector<std::unique_ptr<EvalModule>> mods;
    mods.push_back(std::unique_ptr<EvalModule>(a));
    mods.push_back(std::unique_ptr<EvalModule>(b));

    EvalRunner runner(std::move(mods), nullptr);
    runner.start();

    runner.push_round_start(GameStateSnapshot{});

    wait_for(*a, 1);
    wait_for(*b, 1);

    runner.stop();

    CHECK(a->count() == 1);
    CHECK(b->count() == 1);
}

// T3: Full queue drops event and does not block caller.
TEST_CASE("EvalRunner drops event when queue full") {
    constexpr size_t cap = 4;

    auto* rec = new RecordingModule;
    std::vector<std::unique_ptr<EvalModule>> mods;
    mods.push_back(std::unique_ptr<EvalModule>(rec));

    // Don't call start() so the worker never drains — queue fills up.
    EvalRunner runner(std::move(mods), nullptr, cap);

    for (size_t i = 0; i < cap; ++i)
        runner.push_trade(make_trade(static_cast<int32_t>(i)));

    // Measure that the (cap+1)-th push returns quickly despite a full queue.
    auto t0 = std::chrono::steady_clock::now();
    runner.push_trade(make_trade(99));
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    CHECK(elapsed_ms < 10);
    CHECK(runner.dropped_count() >= 1);
}

// T4: Modules receive events in FIFO order.
TEST_CASE("EvalRunner delivers events in FIFO order") {
    auto* rec = new RecordingModule;
    std::vector<std::unique_ptr<EvalModule>> mods;
    mods.push_back(std::unique_ptr<EvalModule>(rec));

    EvalRunner runner(std::move(mods), nullptr);
    runner.start();

    runner.push_trade(make_trade(10));
    runner.push_trade(make_trade(20));
    runner.push_trade(make_trade(30));

    wait_for(*rec, 3);
    runner.stop();

    REQUIRE(rec->trade_prices.size() == 3);
    CHECK(rec->trade_prices[0] == 10);
    CHECK(rec->trade_prices[1] == 20);
    CHECK(rec->trade_prices[2] == 30);
}
