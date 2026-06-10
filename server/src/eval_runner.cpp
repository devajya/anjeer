#include "server/eval/eval_runner.h"

#include <chrono>
#include <thread>

namespace anjeer::server::eval {

EvalRunner::EvalRunner(std::vector<std::unique_ptr<EvalModule>> modules,
                       EvalOutputCallback                       output_cb,
                       size_t                                   queue_capacity)
    : queue_(queue_capacity)
    , queue_capacity_(queue_capacity)
    , modules_(std::move(modules))
    , output_cb_(std::move(output_cb))
{
    for (auto& m : modules_)
        m->set_output_cb(output_cb_);
}

EvalRunner::~EvalRunner() {
    stop();
}

void EvalRunner::init_session(const std::array<engine::DeckSpec, 12>& deck_specs) {
    for (auto& m : modules_)
        m->on_session_init(deck_specs);
}

void EvalRunner::start() {
    running_.store(true, std::memory_order_release);
    worker_ = std::thread([this] { run_loop(); });
}

void EvalRunner::stop() {
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
}

bool EvalRunner::try_push(TaggedEvent ev) {
    if (queue_size_.load(std::memory_order_relaxed) >= queue_capacity_) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    queue_size_.fetch_add(1, std::memory_order_relaxed);
    queue_.enqueue(std::move(ev));
    return true;
}

void EvalRunner::push_round_start(const engine::GameStateSnapshot& snap) {
    try_push({TaggedEvent::RoundStart, snap});
}

void EvalRunner::push_trade(const EvalTradeEvent& ev) {
    try_push({TaggedEvent::Trade, ev});
}

void EvalRunner::push_book_update(const EvalBookUpdate& ev) {
    try_push({TaggedEvent::Book, ev});
}

void EvalRunner::push_round_end(const engine::GameStateSnapshot& snap) {
    try_push({TaggedEvent::RoundEnd, snap});
}

void EvalRunner::push_order_added(const EvalOrderAdded& ev) {
    try_push({TaggedEvent::OrderAdded, ev});
}

void EvalRunner::push_order_cancelled(const EvalOrderCancelled& ev) {
    try_push({TaggedEvent::OrderCancelled, ev});
}

void EvalRunner::run_loop() {
    while (running_.load(std::memory_order_acquire)) {
        TaggedEvent ev;
        bool drained = false;
        while (queue_.try_dequeue(ev)) {
            queue_size_.fetch_sub(1, std::memory_order_relaxed);
            try {
                dispatch(ev);
            } catch (const std::exception& ex) {
                fprintf(stderr, "[eval] dispatch exception: %s\n", ex.what());
                fflush(stderr);
            } catch (...) {
                fprintf(stderr, "[eval] dispatch unknown exception\n");
                fflush(stderr);
            }
            drained = true;
        }
        if (!drained) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    // Drain any remaining events pushed before stop().
    TaggedEvent ev;
    while (queue_.try_dequeue(ev)) {
        queue_size_.fetch_sub(1, std::memory_order_relaxed);
        try { dispatch(ev); } catch (...) {}
    }
}

void EvalRunner::dispatch(const TaggedEvent& te) {
    int module_idx = 0;
    for (auto& m : modules_) {
        try {
            switch (te.tag) {
                case TaggedEvent::RoundStart:
                    m->on_round_start(std::get<engine::GameStateSnapshot>(te.ev));
                    break;
                case TaggedEvent::Trade:
                    m->on_trade_event(std::get<EvalTradeEvent>(te.ev));
                    break;
                case TaggedEvent::Book:
                    m->on_book_update(std::get<EvalBookUpdate>(te.ev));
                    break;
                case TaggedEvent::RoundEnd:
                    m->on_round_end(std::get<engine::GameStateSnapshot>(te.ev));
                    break;
                case TaggedEvent::OrderAdded:
                    m->on_order_added(std::get<EvalOrderAdded>(te.ev));
                    break;
                case TaggedEvent::OrderCancelled:
                    m->on_order_cancelled(std::get<EvalOrderCancelled>(te.ev));
                    break;
            }
        } catch (const std::exception& ex) {
            fprintf(stderr, "[eval] module[%d] exception tag=%d: %s\n",
                    module_idx, static_cast<int>(te.tag), ex.what());
            fflush(stderr);
        } catch (...) {
            fprintf(stderr, "[eval] module[%d] unknown exception tag=%d\n",
                    module_idx, static_cast<int>(te.tag));
            fflush(stderr);
        }
        ++module_idx;
    }
}

} // namespace anjeer::server::eval
