#pragma once

#include "engine/game_snapshot.h"
#include "server/eval/eval_types.h"

namespace anjeer::server::eval {

class EvalModule {
public:
    virtual ~EvalModule() = default;

    // EvalRunner calls this once before the first on_* callback.
    void set_output_cb(EvalOutputCallback cb) { output_cb_ = std::move(cb); }

    virtual void on_round_start(const engine::GameStateSnapshot&) = 0;
    virtual void on_trade_event(const EvalTradeEvent&)            = 0;
    virtual void on_book_update(const EvalBookUpdate&)            = 0;
    virtual void on_round_end  (const engine::GameStateSnapshot&) = 0;

protected:
    void emit(EvalOutput out) { if (output_cb_) output_cb_(std::move(out)); }

private:
    EvalOutputCallback output_cb_;
};

} // namespace anjeer::server::eval
