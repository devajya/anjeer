#pragma once

#include "engine/game_snapshot.h"
#include "server/eval/eval_module.h"
#include "server/eval/eval_types.h"

#include <readerwriterqueue.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <variant>
#include <vector>

namespace anjeer::server::eval {

// AGENT-CTX: EvalRunner runs on a dedicated worker thread — push_* methods are
// called from the game-loop thread; they are non-blocking and drop events when
// the queue is at capacity to avoid back-pressure on the game loop. The worker
// thread drains the queue and dispatches to all registered modules in order.
// queue_capacity enforced via a separate atomic counter (ReaderWriterQueue grows
// dynamically; the counter is the actual bound).
class EvalRunner {
public:
    EvalRunner(std::vector<std::unique_ptr<EvalModule>> modules,
               EvalOutputCallback                       output_cb,
               size_t                                   queue_capacity = 4096);
    ~EvalRunner();

    void start();
    void stop();

    // Non-blocking: drops silently if queue is at capacity.
    void push_round_start(const engine::GameStateSnapshot&);
    void push_trade      (const EvalTradeEvent&);
    void push_book_update(const EvalBookUpdate&);
    void push_round_end  (const engine::GameStateSnapshot&);

    // Monotonically increasing count of dropped events (for diagnostics / tests).
    size_t dropped_count() const { return dropped_.load(std::memory_order_relaxed); }

private:
    struct TaggedEvent {
        enum Tag { RoundStart, Trade, Book, RoundEnd } tag;
        std::variant<engine::GameStateSnapshot, EvalTradeEvent, EvalBookUpdate> ev;
    };

    bool try_push(TaggedEvent ev);

    void run_loop();
    void dispatch(const TaggedEvent&);

    moodycamel::ReaderWriterQueue<TaggedEvent> queue_;
    std::atomic<size_t>                        queue_size_{0};
    size_t                                     queue_capacity_;
    std::atomic<size_t>                        dropped_{0};

    std::vector<std::unique_ptr<EvalModule>> modules_;
    EvalOutputCallback                       output_cb_;
    std::thread                              worker_;
    std::atomic<bool>                        running_{false};
};

} // namespace anjeer::server::eval
