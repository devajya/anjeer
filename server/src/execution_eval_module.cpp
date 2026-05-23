#include "server/eval/execution_eval_module.h"

#include "engine/suit.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <string>

namespace anjeer::server::eval {

// AGENT-CTX: on_trade_event and on_book_update are no-ops until Task 18.
// compute_and_emit() emits a zero-valued JSON skeleton (Task 17) so T17 passes
// and the wiring is verified. Math helpers are filled in during Task 18.

void ExecutionEvalModule::on_round_start(const engine::GameStateSnapshot& snap) {
    last_snap_ = snap;
    num_active_slots_ = snap.num_active_slots;
    suit_stats_ = {};
}

void ExecutionEvalModule::on_trade_event(const EvalTradeEvent& t) {
    const int  idx = engine::suit_index(t.suit);
    SuitStats& ss  = suit_stats_[idx];

    // fill_rate: count-based EWMA (each trade contributes 1.0) → naturally in [0,1].
    ss.fill_rate = EWMA_ALPHA * 1.0 + (1.0 - EWMA_ALPHA) * ss.fill_rate;

    // trade_intensity: rate-based EWMA in trades/s; 100 ms default for the first trade.
    const int64_t interval_ms = (ss.last_trade_ts_ms < 0)
                                ? 100LL
                                : (t.timestamp_ms - ss.last_trade_ts_ms);
    if (interval_ms > 0) {
        const double rate  = 1000.0 / static_cast<double>(interval_ms);
        ss.trade_intensity = EWMA_ALPHA * rate + (1.0 - EWMA_ALPHA) * ss.trade_intensity;
    }

    ++ss.recent_trades;
    ss.last_trade_ts_ms = t.timestamp_ms;

    // Decay leakage on every trade across all suits.
    for (SuitStats& s : suit_stats_)
        s.leakage_penalty *= LEAKAGE_DECAY;

    // Directional cross: buyer lifts the ask → information leakage signal.
    if (ss.best_ask.has_value() && t.price >= *ss.best_ask)
        ss.leakage_penalty += LEAKAGE_STEP;
}

void ExecutionEvalModule::on_book_update(const EvalBookUpdate& bu) {
    // AGENT-CTX: Book state cached here; EWMA updates deferred to Task 18.
    const int si = engine::suit_index(bu.suit);
    if (bu.best_bid && bu.best_ask)
        suit_stats_[si].spread_width = *bu.best_ask - *bu.best_bid;
    if (bu.best_bid)  suit_stats_[si].best_bid = bu.best_bid;
    if (bu.best_ask)  suit_stats_[si].best_ask = bu.best_ask;
    compute_and_emit();
}

void ExecutionEvalModule::on_round_end(const engine::GameStateSnapshot& snap) {
    last_snap_ = snap;
    compute_and_emit();
}

double ExecutionEvalModule::fill_probability_for(int suit_idx) const {
    return estimate_fill_probability(suit_stats_[suit_idx]);
}

double ExecutionEvalModule::estimate_fill_probability(const SuitStats& s) const {
    if (s.spread_width <= 0) return 0.0;
    return std::clamp(
        s.fill_rate * std::exp(-static_cast<double>(s.spread_width) / FILL_DECAY_K),
        0.0, 1.0);
}

double ExecutionEvalModule::compute_execution_cost(int best_ask, double modeled_ev) const {
    return static_cast<double>(best_ask) - modeled_ev;
}

void ExecutionEvalModule::compute_and_emit() {
    using nlohmann::json;

    json suits = json::object();
    for (int si = 0; si < 4; ++si) {
        const SuitStats& s   = suit_stats_[si];
        const std::string key(engine::suit_name(engine::kAllSuits[si]));

        const double fp = estimate_fill_probability(s);
        // proxy fair value = best_bid when no posterior available
        const double fair_value = s.best_bid ? static_cast<double>(*s.best_bid) : 0.0;
        const double agg_cost   = s.best_ask
                                      ? compute_execution_cost(*s.best_ask, fair_value)
                                      : 0.0;
        const double passive_ev = fp * (s.spread_width / 2.0) - s.leakage_penalty;

        // Recommendation
        std::string rec;
        if (s.spread_width == 0 || (!s.best_bid && !s.best_ask)) {
            rec = "hold";
        } else if (passive_ev > 0.0) {
            rec = "passive";
        } else {
            rec = "aggressive";
        }

        // Liquidity tiers: thin < 2 trades, normal < 10, deep >= 10
        const std::string liq = s.recent_trades < 2  ? "thin"
                              : s.recent_trades < 10 ? "normal"
                                                     : "deep";

        // Risk tiers: low fp > 0.6, moderate > 0.3, elevated otherwise
        const std::string risk = fp > 0.6 ? "low"
                               : fp > 0.3 ? "moderate"
                                          : "elevated";

        suits[key] = {
            {"aggressive_buy_cost", agg_cost},
            {"passive_ev",          passive_ev},
            {"fill_probability",    fp},
            {"recommendation",      rec},
            {"liquidity",           liq},
            {"spread_width",        s.spread_width},
            {"trade_intensity",     s.trade_intensity < 1.0 ? "low"
                                  : s.trade_intensity < 5.0 ? "moderate"
                                                            : "high"},
            {"execution_risk",      risk},
        };
    }

    EvalOutput out;
    out.type        = EvalOutput::Type::ExecutionGuidance;
    out.target_slot = -1;
    out.payload     = {{"type", "eval.execution_guidance"}, {"suits", suits}};
    emit(std::move(out));
}

} // namespace anjeer::server::eval
