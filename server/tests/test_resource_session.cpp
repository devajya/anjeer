#include <catch2/catch_test_macros.hpp>

#include "server/bot_scheduler.h"
#include "server/eval/eval_module.h"
#include "server/eval/eval_runner.h"
#include "server/eval/eval_types.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using namespace anjeer::server;
using namespace anjeer::server::eval;
using namespace std::chrono_literals;

static EvalOutputCallback noop_cb() {
    return [](EvalOutput) {};
}

// ── Class 1: thread-lifetime tests (no DB, no network) ───────────────────────

// EvalRunner::stop() must join its worker without hanging.
TEST_CASE("EvalRunner start+stop does not hang", "[resource][session]") {
    auto runner = std::make_unique<EvalRunner>(
        std::vector<std::unique_ptr<EvalModule>>{},
        noop_cb()
    );
    runner->start();
    REQUIRE_NOTHROW(runner->stop());
}

// Repeated start/stop cycles must not leak threads or hang.
TEST_CASE("EvalRunner multiple start+stop cycles are clean", "[resource][session]") {
    for (int i = 0; i < 3; ++i) {
        EvalRunner runner(
            std::vector<std::unique_ptr<EvalModule>>{},
            noop_cb()
        );
        runner.start();
        REQUIRE_NOTHROW(runner.stop());
    }
}

// EvalRunner destructor without explicit stop must not hang.
TEST_CASE("EvalRunner destructor stops worker thread", "[resource][session]") {
    REQUIRE_NOTHROW([]() {
        EvalRunner runner(
            std::vector<std::unique_ptr<EvalModule>>{},
            noop_cb()
        );
        runner.start();
        // implicit stop via destructor
    }());
}

// BotScheduler deregister_bot does not leave a zombie callback.
TEST_CASE("BotScheduler deregistered bot does not fire after deregister", "[resource][session]") {
    BotScheduler sched(2);

    std::atomic<int> fires{0};
    auto h = sched.register_bot([&]{ ++fires; }, 50);
    std::this_thread::sleep_for(200ms);
    int before = fires.load();
    REQUIRE(before > 0);

    sched.deregister_bot(h);
    int after_deregister = fires.load();
    std::this_thread::sleep_for(200ms);
    REQUIRE(fires.load() == after_deregister);
}
