#pragma once

#include "engine/game_snapshot.h"
#include "server/eval/eval_types.h"

namespace anjeer::server::eval {

class EvalModule {
public:
    virtual ~EvalModule() = default;

    // EvalRunner calls this once before the first on_* callback.
    void set_output_cb(EvalOutputCallback cb) { output_cb_ = std::move(cb); }

    // Called once at session creation with the static deck table.
    // Modules that need the deck table (BayesianEvalModule) store it here
    // rather than re-copying it from every snapshot.
    virtual void on_session_init(const std::array<engine::DeckSpec, 12>&) {}

    virtual void on_round_start(const engine::GameStateSnapshot&) = 0;
    virtual void on_trade_event(const EvalTradeEvent&)            = 0;
    virtual void on_book_update(const EvalBookUpdate&)            = 0;
    virtual void on_round_end  (const engine::GameStateSnapshot&) = 0;

protected:
    void emit(EvalOutput out) { if (output_cb_) output_cb_(std::move(out)); }
    bool has_output_cb() const noexcept { return static_cast<bool>(output_cb_); }

private:
    EvalOutputCallback output_cb_;
};

} // namespace anjeer::server::eval
