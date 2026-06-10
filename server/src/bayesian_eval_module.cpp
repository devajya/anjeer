#include "server/eval/bayesian_eval_module.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace anjeer::server::eval {

using namespace anjeer::engine;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

BayesianEvalModule::BayesianEvalModule() {
    precompute_log_factorials();
}

void BayesianEvalModule::on_session_init(const std::array<DeckSpec, 12>& table) {
    deck_table_            = table;
    deck_table_initialized_ = true;
}

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
    if (!deck_table_initialized_)
        deck_table_ = snap.deck_table;  // test-compat fallback; production uses on_session_init

    server_hands_                   = snap.hands;
    observed_deltas_         = {};
    time_remaining_s_        = snap.time_remaining_s;
    round_duration_approx_s_ = snap.time_remaining_s;
    points_per_card_         = snap.points_per_card;

    for (int slot = 0; slot < 4; ++slot) {
        if (!snap.slot_active[slot]) {
            posteriors_[slot].fill(0.0);
            continue;
        }

        // Log-likelihoods for each deck config
        std::array<double, 12> log_w{};
        for (int i = 0; i < 12; ++i)
            log_w[i] = hypergeometric_log_likelihood(snap.hands[slot], deck_table_[i]);

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
    constexpr double TAU                 = 60.0;  // seconds; controls decay midpoint
    constexpr double HEURISTIC_BOOST     = 0.3;   // base log-space nudge per trade
    constexpr double SELL_EVIDENCE_RATIO = 0.5;   // sell is weaker signal than buy by default
    constexpr double MAX_NUDGE           = 0.25;  // clamp prevents posterior collapse

    const double t_remaining = std::max(0.0, round_duration_approx_s_ - ev.timestamp_ms / 1000.0);
    const double weight      = t_remaining / (t_remaining + TAU);
    const double qty_scale   = std::sqrt(static_cast<double>(std::max(1, ev.qty)));
    const int    si          = suit_index(ev.suit);

    // ── Buyer signal ──────────────────────────────────────────────────────────
    const int buyer = ev.buyer_slot;
    if (buyer >= 0 && buyer < 4) {
        const double delta = std::clamp(weight * HEURISTIC_BOOST * qty_scale, 0.0, MAX_NUDGE);
        for (int i = 0; i < 12; ++i)
            if (deck_table_[i].goal_suit == ev.suit)
                posteriors_[buyer][i] *= std::exp(delta);
        normalize(posteriors_[buyer]);
    }

    // ── Seller signal ─────────────────────────────────────────────────────────
    // Strength is inverse of the seller's publicly observed accumulation in this
    // suit (InfoTier::ObserverDerived). Selling from a large observed position is
    // weak anti-goal evidence; selling from an observed zero position is strong.
    // Uses observed_deltas_, never server_hands_, so no private data crosses tiers.
    const int seller = ev.seller_slot;
    if (seller >= 0 && seller < 4) {
        const int obs_holding = std::max(0, observed_deltas_[seller][si]);
        const double sell_strength = SELL_EVIDENCE_RATIO * HEURISTIC_BOOST
                                   / (1.0 + static_cast<double>(obs_holding));
        const double delta = std::clamp(weight * sell_strength * qty_scale, 0.0, MAX_NUDGE);
        for (int i = 0; i < 12; ++i)
            if (deck_table_[i].goal_suit == ev.suit)
                posteriors_[seller][i] *= std::exp(-delta);
        normalize(posteriors_[seller]);
    }
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
            settlement_ev += posteriors_[slot][i] * server_hands_[slot][g] * points_per_card_;
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
    const int si = suit_index(ev.suit);
    if (ev.buyer_slot >= 0 && ev.buyer_slot < 4) {
        server_hands_[ev.buyer_slot][si]    = std::max(0, server_hands_[ev.buyer_slot][si] + ev.qty);
        observed_deltas_[ev.buyer_slot][si] += ev.qty;
    }
    if (ev.seller_slot >= 0 && ev.seller_slot < 4) {
        server_hands_[ev.seller_slot][si]    = std::max(0, server_hands_[ev.seller_slot][si] - ev.qty);
        observed_deltas_[ev.seller_slot][si] -= ev.qty;
    }
    apply_trade_heuristic(ev);
    emit_all(time_remaining_s_);
}

void BayesianEvalModule::on_book_update(const EvalBookUpdate&) {
    // Book updates carry no hand-inference signal for Bayesian updating.
}

void BayesianEvalModule::on_round_end(const GameStateSnapshot& snap) {
    server_hands_           = snap.hands;
    points_per_card_ = snap.points_per_card;
    emit_all(snap.time_remaining_s);
}

} // namespace anjeer::server::eval
