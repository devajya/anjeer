#pragma once

#include "server/eval/eval_module.h"
#include "server/eval/eval_types.h"
#include "engine/game_snapshot.h"

#include <array>
#include <optional>

namespace anjeer::server::eval {

// Computes per-suit execution guidance: fill probability, aggressive vs passive
// EV comparison, spread cost, and a trade recommendation for each suit.
//
// fill_probability = fill_rate_ewma × exp(-spread_width / k), clamped [0,1].
// aggressive_buy_cost = best_ask - modeled_ev (approximated as best_bid pre-Bayesian).
// passive_ev = fill_prob × (spread_width / 2.0) - leakage_penalty.
// recommendation: "passive" if passive_ev > aggressive_buy_cost, else "aggressive";
//   "hold" when spread == 0 or no market (no best_bid/best_ask).
//
// Emits eval.execution_guidance (public, target_slot == -1) via cb_ after
// every book_update and round_end event.
//
// AGENT-CTX: on_* bodies are intentionally no-ops until Task 18.
// set_output_cb() mirrors AccumulationEvalModule pattern so tests can inject a
// callback without requiring a constructor argument.
class ExecutionEvalModule : public EvalModule {
public:
    ExecutionEvalModule() = default;

    void on_round_start(const engine::GameStateSnapshot&) override;
    void on_trade_event(const EvalTradeEvent&)            override;
    void on_book_update(const EvalBookUpdate&)            override;
    void on_round_end  (const engine::GameStateSnapshot&) override;

    void set_output_cb(EvalOutputCallback cb) { EvalModule::set_output_cb(std::move(cb)); }

    // Test inspectors — expose per-suit internal state without coupling to JSON
    double fill_probability_for(int suit_idx) const;
    double trade_intensity_for (int suit_idx) const { return suit_stats_[suit_idx].trade_intensity; }
    int    spread_width_for    (int suit_idx) const { return suit_stats_[suit_idx].spread_width; }
    double leakage_penalty_for (int suit_idx) const { return suit_stats_[suit_idx].leakage_penalty; }
    int    recent_trades_for   (int suit_idx) const { return suit_stats_[suit_idx].recent_trades; }

private:
    struct SuitStats {
        double fill_rate{0.0};          // ewma of inverse inter-trade interval (fills/s)
        double trade_intensity{0.0};    // ewma of trades per second
        int    spread_width{0};         // best_ask - best_bid; 0 means no two-sided market
        int    recent_trades{0};        // cumulative this round
        double leakage_penalty{0.0};    // cost proxy for directional crossing — private to module

        std::optional<int32_t> best_bid;
        std::optional<int32_t> best_ask;
        int64_t last_trade_ts_ms{-1};   // timestamp of last trade, -1 = none
    };

    std::array<SuitStats, 4>  suit_stats_{};
    engine::GameStateSnapshot last_snap_{};
    int                       num_active_slots_{4};

    // fill_rate EWMA decay constant: e^(-spread/k) → k=5 balances wide-spread penalty
    static constexpr double FILL_DECAY_K    = 5.0;
    static constexpr double EWMA_ALPHA      = 0.1;
    static constexpr double LEAKAGE_STEP    = 0.5;  // added per directional cross
    static constexpr double LEAKAGE_DECAY   = 0.95; // applied per trade event

    double estimate_fill_probability(const SuitStats&) const;
    double compute_execution_cost   (int best_ask, double modeled_ev) const;
    void   compute_and_emit();
};

} // namespace anjeer::server::eval
