#include "server/eval/accumulation_eval_module.h"

namespace anjeer::server::eval {

void AccumulationEvalModule::set_output_cb(EvalOutputCallback cb) { cb_ = std::move(cb); }

void AccumulationEvalModule::on_round_start(const engine::GameStateSnapshot&) {}
void AccumulationEvalModule::on_trade_event(const EvalTradeEvent&) {}
void AccumulationEvalModule::on_book_update(const EvalBookUpdate&) {}
void AccumulationEvalModule::on_round_end(const engine::GameStateSnapshot&) {}
void AccumulationEvalModule::compute_and_emit() {}

} // namespace anjeer::server::eval
