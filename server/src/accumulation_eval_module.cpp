#include "server/eval/accumulation_eval_module.h"
#include "engine/suit.h"

#include <nlohmann/json.hpp>
#include <cmath>
#include <algorithm>

namespace anjeer::server::eval {

void AccumulationEvalModule::on_round_start(const engine::GameStateSnapshot& snap) {
    signed_deltas_    = {};
    num_active_slots_ = snap.num_active_slots;
    player_names_     = snap.player_names;
    // EWMA baseline intentionally persists across rounds to normalise for
    // overall market activity level observed in previous rounds.
    compute_and_emit();
}

void AccumulationEvalModule::on_trade_event(const EvalTradeEvent& ev) {
    const int si  = engine::suit_index(ev.suit);
    const int cap = static_cast<int>(signed_deltas_.size());
    if (ev.buyer_slot  >= 0 && ev.buyer_slot  < cap) signed_deltas_[ev.buyer_slot][si]  += 1;
    if (ev.seller_slot >= 0 && ev.seller_slot < cap) signed_deltas_[ev.seller_slot][si] -= 1;

    // Track absolute per-trade magnitude for baseline normalisation.
    // Each trade contributes 1.0 of absolute flow to the dominant suit.
    ewma_baseline_[si] = ewma_alpha_ * 1.0 + (1.0 - ewma_alpha_) * ewma_baseline_[si];

    compute_and_emit();
}

void AccumulationEvalModule::on_book_update(const EvalBookUpdate&) {}

void AccumulationEvalModule::on_round_end(const engine::GameStateSnapshot&) {
    compute_and_emit();
}

void AccumulationEvalModule::compute_and_emit() {
    nlohmann::json players = nlohmann::json::array();

    const int n = std::min(num_active_slots_, static_cast<int>(signed_deltas_.size()));
    for (int slot = 0; slot < n; ++slot) {
        const auto& deltas = signed_deltas_[slot];

        double max_abs = 0.0;
        double sum_abs = 0.0;
        int    primary_suit_idx = 0;
        for (int s = 0; s < 4; ++s) {
            const double abs_d = std::abs(deltas[s]);
            sum_abs += abs_d;
            if (abs_d > max_abs) {
                max_abs          = abs_d;
                primary_suit_idx = s;
            }
        }

        // Purity: fraction of flow concentrated in the dominant suit [0,1].
        // Equals 1.0 when all flow is in one suit; ~0.25 when perfectly uniform.
        const double purity = max_abs / (sum_abs + 1e-9);

        // Intensity: saturating function of absolute dominant-suit flow.
        // confidence = purity * (1 - exp(-k * net_strength))
        // This ensures one trade cannot reach High regardless of purity.
        const double confidence = std::max(0.0, std::min(1.0,
            purity * (1.0 - std::exp(-INTENSITY_DECAY_K * max_abs))));

        std::string signal;
        if (confidence >= HIGH_THRESHOLD)          signal = "High";
        else if (confidence >= ELEVATED_THRESHOLD) signal = "Elevated";
        else                                        signal = "Normal";

        players.push_back({
            {"slot",         slot},
            {"player",       player_names_[slot]},
            {"signal",       signal},
            {"confidence",   confidence},
            {"primary_suit", std::string(engine::suit_name(engine::kAllSuits[primary_suit_idx]))}
        });
    }

    emit(EvalOutput{
        EvalOutput::Type::AccumulationSignal,
        -1,
        {{"players", std::move(players)}}
    });
}

} // namespace anjeer::server::eval
