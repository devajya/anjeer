#include "server/eval/execution_eval_module.h"

#include <cmath>

namespace anjeer::server::eval {

// AGENT-CTX: All on_* methods are intentional no-ops until Task 18.
// compute_and_emit() and the math helpers are stubs returning 0 / default.
// Tests in execution_module_tests.cpp compile against these stubs and fail at
// runtime; full implementation fills them in during Task 18.

void ExecutionEvalModule::on_round_start(const engine::GameStateSnapshot& snap) {
    last_snap_ = snap;
    num_active_slots_ = snap.num_active_slots;
    suit_stats_ = {};
}

void ExecutionEvalModule::on_trade_event(const EvalTradeEvent&) {}

void ExecutionEvalModule::on_book_update(const EvalBookUpdate&) {}

void ExecutionEvalModule::on_round_end(const engine::GameStateSnapshot& snap) {
    last_snap_ = snap;
}

double ExecutionEvalModule::fill_probability_for(int suit_idx) const {
    return estimate_fill_probability(suit_stats_[suit_idx]);
}

double ExecutionEvalModule::estimate_fill_probability(const SuitStats& s) const {
    if (s.spread_width <= 0) return 0.0;
    return s.fill_rate * std::exp(-static_cast<double>(s.spread_width) / FILL_DECAY_K);
}

double ExecutionEvalModule::compute_execution_cost(int best_ask, double modeled_ev) const {
    return static_cast<double>(best_ask) - modeled_ev;
}

void ExecutionEvalModule::compute_and_emit() {}

} // namespace anjeer::server::eval
