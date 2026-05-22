#include "server/eval/bayesian_eval_module.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace anjeer::server::eval {

using namespace anjeer::engine;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void BayesianEvalModule::precompute_log_factorials() {
    lf_[0] = 0.0;
    for (int i = 1; i <= 40; ++i)
        lf_[i] = lf_[i - 1] + std::log(static_cast<double>(i));
}

// Multivariate hypergeometric log-likelihood:
//   log P(hand | deck) = Σ_i log C(counts[i], hand[i]) - log C(N, K)
// Returns -inf for any impossible configuration (hand[i] > counts[i]).
double BayesianEvalModule::hypergeometric_log_likelihood(
    const std::array<int, 4>& hand,
    const DeckSpec& deck) const
{
    int N = 0, K = 0;
    for (int i = 0; i < 4; ++i) { N += deck.counts[i]; K += hand[i]; }

    if (K < 0 || K > N) return -std::numeric_limits<double>::infinity();

    // log C(N, K)
    double log_denom = lf_[N] - lf_[K] - lf_[N - K];

    double log_num = 0.0;
    for (int i = 0; i < 4; ++i) {
        int ni = deck.counts[i], ki = hand[i];
        if (ki < 0 || ki > ni) return -std::numeric_limits<double>::infinity();
        if (ki == 0) continue;          // log C(n, 0) = 0
        log_num += lf_[ni] - lf_[ki] - lf_[ni - ki];
    }

    return log_num - log_denom;
}

void BayesianEvalModule::normalize(std::array<double, 12>& p) {
    double total = 0.0;
    for (double x : p) total += x;
    if (total > 0.0) {
        for (double& x : p) x /= total;
    }
}

// ---------------------------------------------------------------------------
// init_from_snapshot — compute posteriors for all active slots
// ---------------------------------------------------------------------------

void BayesianEvalModule::init_from_snapshot(const GameStateSnapshot& snap) {
    precompute_log_factorials();   // idempotent; required before lf_ is used

    deck_table_       = snap.deck_table;
    hands_            = snap.hands;
    time_remaining_s_ = snap.time_remaining_s;
    points_per_card_  = snap.points_per_card;

    for (int slot = 0; slot < 4; ++slot) {
        if (!snap.slot_active[slot]) {
            posteriors_[slot].fill(0.0);
            continue;
        }

        // Log-likelihoods for each deck config
        std::array<double, 12> log_w{};
        for (int i = 0; i < 12; ++i)
            log_w[i] = hypergeometric_log_likelihood(snap.hands[slot], snap.deck_table[i]);

        // Log-sum-exp shift to prevent underflow when all likelihoods are small
        const double max_ll = *std::max_element(log_w.begin(), log_w.end());

        std::array<double, 12> w{};
        for (int i = 0; i < 12; ++i)
            w[i] = std::exp(log_w[i] - max_ll);

        normalize(w);
        posteriors_[slot] = w;
    }
}

// ---------------------------------------------------------------------------
// apply_trade_heuristic
//
// Buyer accumulating suit S → upweights deck configs whose goal_suit == S for
// that slot. The log-space nudge is scaled by a weight that is an increasing
// function of time_remaining_s_: early in the round trades are more informative
// because players pursue strategies openly; late in the round they may hedge.
//
// Weight formula: w = t / (t + TAU), giving w(120s)≈0.67, w(2s)≈0.03 —
// a clear gradient that satisfies the T11 spread-ordering requirement.
// ---------------------------------------------------------------------------
void BayesianEvalModule::apply_trade_heuristic(const EvalTradeEvent& ev) {
    const double TAU            = 60.0;   // seconds; controls midpoint of curve
    const double HEURISTIC_BOOST = 0.3;   // log-space nudge per trade

    const double weight = time_remaining_s_ / (time_remaining_s_ + TAU);

    const int buyer = ev.buyer_slot;
    if (buyer < 0 || buyer >= 4) return;

    // Multiply matching configs by exp(weight * BOOST), then renormalize.
    for (int i = 0; i < 12; ++i) {
        if (deck_table_[i].goal_suit == ev.suit)
            posteriors_[buyer][i] *= std::exp(weight * HEURISTIC_BOOST);
    }
    normalize(posteriors_[buyer]);
}

// ---------------------------------------------------------------------------
// emit_all — fires EvalOutput{PosteriorUpdate} once per active slot
// ---------------------------------------------------------------------------
void BayesianEvalModule::emit_all(double /*time_remaining_s*/) {

    for (int slot = 0; slot < 4; ++slot) {
        // Skip slots whose posterior was never initialised (inactive)
        {
            double total = 0.0;
            for (double x : posteriors_[slot]) total += x;
            if (total < 1e-12) continue;
        }

        nlohmann::json payload;

        // configurations[] — 12-entry array with probabilities
        nlohmann::json configs = nlohmann::json::array();
        for (int i = 0; i < 12; ++i) {
            const auto& counts = deck_table_[i].counts;
            configs.push_back({
                {"deck_index",  i},
                {"counts",      {counts[0], counts[1], counts[2], counts[3]}},
                {"goal_suit",   std::string(suit_name(deck_table_[i].goal_suit))},
                {"probability", posteriors_[slot][i]}
            });
        }
        payload["configurations"] = std::move(configs);

        // goal_suit_marginals — Σ posteriors per goal suit
        std::array<double, 4> marginals{};
        for (int i = 0; i < 12; ++i)
            marginals[suit_index(deck_table_[i].goal_suit)] += posteriors_[slot][i];

        nlohmann::json marg;
        for (auto s : kAllSuits)
            marg[std::string(suit_name(s))] = marginals[suit_index(s)];
        payload["goal_suit_marginals"] = std::move(marg);

        // settlement_ev = Σ_i P(config_i) · hands[slot][goal_suit(config_i)] · ppc
        double settlement_ev = 0.0;
        for (int i = 0; i < 12; ++i) {
            const int g = suit_index(deck_table_[i].goal_suit);
            settlement_ev += posteriors_[slot][i] * hands_[slot][g] * points_per_card_;
        }
        payload["settlement_ev"] = settlement_ev;

        // delta_ev[suit] = Σ_{i: goal(i)==suit} P(config_i) · ppc
        // Represents the marginal EV gain of acquiring one more card of that suit.
        nlohmann::json dev;
        for (auto s : kAllSuits) {
            double d = 0.0;
            for (int i = 0; i < 12; ++i) {
                if (deck_table_[i].goal_suit == s)
                    d += posteriors_[slot][i] * points_per_card_;
            }
            dev[std::string(suit_name(s))] = d;
        }
        payload["delta_ev"] = std::move(dev);

        emit(EvalOutput{EvalOutput::Type::PosteriorUpdate, slot, std::move(payload)});
    }
}

// ---------------------------------------------------------------------------
// EvalModule interface
// ---------------------------------------------------------------------------

void BayesianEvalModule::on_round_start(const GameStateSnapshot& snap) {
    init_from_snapshot(snap);
    emit_all(snap.time_remaining_s);
}

void BayesianEvalModule::on_trade_event(const EvalTradeEvent& ev) {
    apply_trade_heuristic(ev);
    emit_all(time_remaining_s_);
}

void BayesianEvalModule::on_book_update(const EvalBookUpdate&) {
    // Book updates carry no hand-inference signal for Bayesian updating.
}

void BayesianEvalModule::on_round_end(const GameStateSnapshot& snap) {
    hands_           = snap.hands;
    points_per_card_ = snap.points_per_card;
    emit_all(snap.time_remaining_s);
}

} // namespace anjeer::server::eval
