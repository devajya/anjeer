#include "server/eval/execution_eval_module.h"

#include "engine/suit.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <string>

namespace anjeer::server::eval {

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

    compute_and_emit();
}

void ExecutionEvalModule::on_book_update(const EvalBookUpdate& bu) {
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

double ExecutionEvalModule::passive_ev_for(int suit_idx) const {
    const SuitStats& s = suit_stats_[suit_idx];
    return estimate_fill_probability(s) * (s.spread_width / 2.0) - s.leakage_penalty;
}

double ExecutionEvalModule::aggressive_buy_cost_for(int suit_idx) const {
    const SuitStats& s  = suit_stats_[suit_idx];
    const double fair   = s.best_bid ? static_cast<double>(*s.best_bid) : 0.0;
    return s.best_ask ? compute_execution_cost(*s.best_ask, fair) : 0.0;
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

    struct SuitResult {
        std::string            rec;
        double                 passive_ev{0.0};
        double                 fp{0.0};
        std::optional<int32_t> best_bid;
        std::optional<int32_t> best_ask;
    };
    std::array<SuitResult, 4> results{};

    for (int si = 0; si < 4; ++si) {
        const SuitStats& s = suit_stats_[si];
        SuitResult&      r = results[si];

        r.fp       = estimate_fill_probability(s);
        r.best_bid = s.best_bid;
        r.best_ask = s.best_ask;

        const double fair_value = s.best_bid ? static_cast<double>(*s.best_bid) : 0.0;
        (void)compute_execution_cost(s.best_ask ? *s.best_ask : 0, fair_value); // kept for future use
        r.passive_ev = r.fp * (s.spread_width / 2.0) - s.leakage_penalty;

        if (s.spread_width == 0 || (!s.best_bid && !s.best_ask))
            r.rec = "hold";
        else if (r.passive_ev > 0.0)
            r.rec = "passive";
        else
            r.rec = "aggressive";
    }

    // Build per-suit JSON object for the frontend table.
    json suits_json = json::object();
    for (int si = 0; si < 4; ++si) {
        const SuitResult& r   = results[si];
        const std::string key(engine::suit_name(engine::kAllSuits[si]));
        const std::string intensity = suit_stats_[si].trade_intensity < 1.0 ? "low"
                                    : suit_stats_[si].trade_intensity < 5.0 ? "moderate"
                                                                             : "high";
        suits_json[key] = {
            {"fill_probability", r.fp},
            {"passive_ev",       r.passive_ev},
            {"trade_intensity",  intensity},
            {"spread_width",     suit_stats_[si].spread_width},
            {"recommendation",   r.rec},
        };
    }

    // Pick the suit with the best opportunity: highest passive_ev (passive),
    // or highest fill_probability (aggressive).
    int    best_si    = -1;
    double best_score = 0.0;
    for (int si = 0; si < 4; ++si) {
        const double score = (results[si].rec == "passive")    ? results[si].passive_ev
                           : (results[si].rec == "aggressive") ? results[si].fp
                                                               : -1.0;
        if (score > best_score) { best_score = score; best_si = si; }
    }

    std::string action = "hold";
    std::string suit;
    json        price  = nullptr;

    if (best_si >= 0) {
        const SuitResult& r = results[best_si];
        suit   = std::string(engine::suit_name(engine::kAllSuits[best_si]));
        action = r.rec; // execution quality: "passive" = quote favorably, "aggressive" = cross
        const auto& target_price = (r.rec == "passive") ? r.best_bid : r.best_ask;
        price = target_price ? json(*target_price) : json(nullptr);
    }

    EvalOutput out;
    out.type        = EvalOutput::Type::ExecutionGuidance;
    out.target_slot = -1;
    out.payload     = {
        {"action", action}, {"suit", suit}, {"price", price},
        {"suits",  suits_json},
    };
    emit(std::move(out));
}

} // namespace anjeer::server::eval
