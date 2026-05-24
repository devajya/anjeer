#pragma once

#include "server/eval/eval_module.h"
#include "server/eval/eval_types.h"
#include "engine/game_snapshot.h"

#include <array>

namespace anjeer::server::eval {

// Maintains a per-slot posterior distribution over the 12 deck configurations,
// updated via multivariate hypergeometric likelihood from observed hands and a
// trade-direction heuristic whose weight decays exponentially with time remaining.
//
// Emits eval.posterior_update (private, per-slot):
//   configurations[]     — 12-entry list with probabilities summing to 1.0
//   goal_suit_marginals  — 4-entry map (suit → P), sum = 1.0
//   settlement_ev        — Σ P(σ) · hands[slot][goal_suit(σ)] · points_per_card
//   delta_ev             — per-suit marginal EV gain of acquiring one more card
//
// set_output_cb() allows tests to inject a callback after default construction
// without requiring a constructor argument — EvalRunner wires it at session init.
class BayesianEvalModule : public EvalModule {
public:
    BayesianEvalModule();

    // Stores the deck table so init_from_snapshot doesn't re-copy it each round.
    // Tests that set snap.deck_table directly don't need to call this.
    void on_session_init(const std::array<engine::DeckSpec, 12>&) override;

    void on_round_start(const engine::GameStateSnapshot&) override;
    void on_trade_event(const EvalTradeEvent&)            override;
    void on_book_update(const EvalBookUpdate&)            override;
    void on_round_end  (const engine::GameStateSnapshot&) override;

    // Test inspector — returns the normalised posterior for one slot over all 12 configs.
    const std::array<double, 12>& posteriors_for(int slot) const { return posteriors_[slot]; }

private:
    // posteriors_[slot][deck_idx] — normalised to sum 1.0 after each update
    std::array<std::array<double, 12>, 4> posteriors_{};
    std::array<engine::DeckSpec, 12>      deck_table_{};
    std::array<std::array<int, 4>, 4>     hands_{};      // [slot][suit_index]
    double                                time_remaining_s_{0.0};
    double                                round_duration_approx_s_{0.0};
    int32_t                               points_per_card_{0};

    bool                                   deck_table_initialized_{false};

    // lf_[n] = log(n!), precomputed for n in [0, 40] at construction
    std::array<double, 41> lf_{};

    void   precompute_log_factorials();
    void   init_from_snapshot(const engine::GameStateSnapshot&);
    double hypergeometric_log_likelihood(const std::array<int, 4>& hand,
                                         const engine::DeckSpec&   deck) const;
    void   normalize(std::array<double, 12>& p);
    void   apply_trade_heuristic(const EvalTradeEvent&);
    void   emit_all(double time_remaining_s);
};

} // namespace anjeer::server::eval
