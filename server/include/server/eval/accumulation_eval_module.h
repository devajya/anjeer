#pragma once

#include "server/eval/eval_module.h"
#include "server/eval/eval_types.h"
#include "engine/game_snapshot.h"

#include <array>
#include <string>

namespace anjeer::server::eval {

// Tracks cumulative per-slot, per-suit card flow (signed deltas) throughout
// a round and classifies each player's accumulation behaviour into one of
// three signal levels: Normal, Elevated, or High.
//
// Concentration score = max(|delta|) / (sum(|deltas|) + ε) for a slot.
// Percentile-ranked against ewma_baseline_ per suit to normalise for round
// activity; thresholds: < 0.65 = Normal, < 0.85 = Elevated, ≥ 0.85 = High.
//
// Emits eval.accumulation_signal (public, target_slot == -1) via cb_ after
// every trade event.
//
class AccumulationEvalModule : public EvalModule {
public:
    AccumulationEvalModule() = default;

    void on_round_start(const engine::GameStateSnapshot&) override;
    void on_trade_event(const EvalTradeEvent&)            override;
    void on_book_update(const EvalBookUpdate&)            override;
    void on_round_end  (const engine::GameStateSnapshot&) override;

    // Test inspectors
    int32_t signed_delta(int slot, int suit) const { return signed_deltas_[slot][suit]; }
    double ewma_baseline_for(int suit)      const { return ewma_baseline_[suit]; }

private:
    // signed_deltas_[slot][suit] — cumulative net card flow this round (always integer ±1/trade)
    std::array<std::array<int32_t, 4>, 4> signed_deltas_{};
    // ewma_baseline_[suit] — exponential moving average of absolute delta magnitude
    std::array<double, 4>                ewma_baseline_{};
    double                               ewma_alpha_{0.1};
    int                                  num_active_slots_{4};
    std::array<std::string, 4>           player_names_{};

    static constexpr double ELEVATED_THRESHOLD = 0.65;
    static constexpr double HIGH_THRESHOLD     = 0.85;
    // Decay constant for intensity saturation curve: confidence = purity*(1-exp(-k*strength))
    // k=0.15 → 1 buy≈0.14 (Normal), 8 buys≈0.70 (Elevated), 20 buys≈0.95 (High)
    static constexpr double INTENSITY_DECAY_K  = 0.15;

    void compute_and_emit();
};

} // namespace anjeer::server::eval
