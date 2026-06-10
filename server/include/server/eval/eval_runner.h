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

class EvalRunner {
public:
    // capacity=64: keeps moodycamel in the single-block path
    // (ceilToPow2(65)=128 <= MAX_BLOCK_SIZE*2=1024, ~109 KB).
    // capacity=4096 triggers 10×426 KB multi-block allocation that
    // fragments the heap and causes bad_alloc on subsequent JSON operations.
    EvalRunner(std::vector<std::unique_ptr<EvalModule>> modules,
               EvalOutputCallback                       output_cb,
               size_t                                   queue_capacity = 64);
    ~EvalRunner();

    void start();
    void stop();

    // Calls on_session_init on all modules — invoke once after construction,
    // before start(), to supply any static session data (e.g. the deck table).
    void init_session(const std::array<engine::DeckSpec, 12>&);

    // Non-blocking: drops silently if queue is at capacity.
    void push_round_start    (const engine::GameStateSnapshot&);
    void push_trade          (const EvalTradeEvent&);
    void push_book_update    (const EvalBookUpdate&);
    void push_round_end      (const engine::GameStateSnapshot&);
    void push_order_added    (const EvalOrderAdded&);
    void push_order_cancelled(const EvalOrderCancelled&);

    // Monotonically increasing count of dropped events (for diagnostics / tests).
    size_t dropped_count() const { return dropped_.load(std::memory_order_relaxed); }

private:
    struct TaggedEvent {
        enum Tag { RoundStart, Trade, Book, RoundEnd, OrderAdded, OrderCancelled } tag;
        std::variant<engine::GameStateSnapshot, EvalTradeEvent, EvalBookUpdate,
                     EvalOrderAdded, EvalOrderCancelled> ev;
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
