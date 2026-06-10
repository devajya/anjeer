#pragma once

#include "engine/game_snapshot.h"
#include "engine/suit.h"
#include "engine/order_book.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace anjeer::engine {

enum class BotDifficulty { Easy, Medium, Hard };

// ── Deck table ────────────────────────────────────────────────────────────────

// All 12 equally-probable deck configurations.
// suit_counts: [clubs, diamonds, hearts, spades] (matching suit_index() order).
// 12-card suit is the same colour as the goal suit but is never the goal itself.
// Goal suit always has 8 or 10 cards.
struct DeckDef {
    std::array<int, 4> suit_counts;  // [C, D, H, S]
    int  goal_suit_index;            // 0=Clubs, 1=Diamonds, 2=Hearts, 3=Spades
    int  majority_bonus;             // 100 or 120 (dollars)
};

// Verified: each row sums to 40, goal suit has 8 or 10 cards.
inline constexpr std::array<DeckDef, 12> DECK_TABLE = {{
    //   C   D   H   S  goal  bonus
    {{ 10,  8, 10, 12},  0,  100},  // d1:  S=12→goal=C(10)
    {{ 10, 10,  8, 12},  0,  100},  // d2:  S=12→goal=C(10)
    {{  8, 10, 10, 12},  0,  120},  // d3:  S=12→goal=C(8)
    {{ 12, 10, 10,  8},  3,  120},  // d4:  C=12→goal=S(8)
    {{ 12,  8, 10, 10},  3,  100},  // d5:  C=12→goal=S(10)
    {{ 12, 10,  8, 10},  3,  100},  // d6:  C=12→goal=S(10)
    {{  8, 10, 12, 10},  1,  100},  // d7:  H=12→goal=D(10)
    {{ 10, 10, 12,  8},  1,  100},  // d8:  H=12→goal=D(10)
    {{ 10,  8, 12, 10},  1,  120},  // d9:  H=12→goal=D(8)
    {{ 10, 12,  8, 10},  2,  120},  // d10: D=12→goal=H(8)
    {{  8, 12, 10, 10},  2,  100},  // d11: D=12→goal=H(10)
    {{ 10, 12, 10,  8},  2,  100},  // d12: D=12→goal=H(10)
}};

// ── Posterior helpers ─────────────────────────────────────────────────────────

// Binomial coefficient C(n, k). Returns 0 for invalid inputs.
inline int64_t binom(int n, int k) {
    if (k < 0 || k > n) return 0;
    if (k == 0 || k == n) return 1;
    if (k > n - k) k = n - k;
    int64_t result = 1;
    for (int i = 0; i < k; ++i) {
        result = result * (n - i) / (i + 1);
    }
    return result;
}

// Compute 12-deck posterior from hand via multivariate hypergeometric likelihood.
// hand: [C, D, H, S] card counts. deck_multiplier scales all DECK_TABLE counts for
// hard-mode games where total card count is multiplied (proportions preserved).
inline std::array<float, 12> compute_hand_posterior(const std::array<int, 4>& hand,
                                                     int deck_multiplier = 1) {
    std::array<float, 12> weights{};
    for (int d = 0; d < 12; ++d) {
        int64_t L = 1;
        for (int s = 0; s < 4; ++s) {
            int64_t c = binom(DECK_TABLE[d].suit_counts[s] * deck_multiplier, hand[s]);
            if (c == 0) { L = 0; break; }
            L *= c;
        }
        weights[d] = static_cast<float>(L);
    }
    float sum = 0.0f;
    for (float w : weights) sum += w;
    if (sum > 0.0f)
        for (float& w : weights) w /= sum;
    else
        weights.fill(1.0f / 12.0f);
    return weights;
}

// Derive P(goal=s) for each suit from deck weights.
inline std::array<float, 4> goal_posteriors(const std::array<float, 12>& weights) {
    std::array<float, 4> P{};
    for (int d = 0; d < 12; ++d)
        P[DECK_TABLE[d].goal_suit_index] += weights[d];
    return P;
}

// Expected majority bonus for suit s at this posterior (equal-split prior).
inline float expected_bonus(const std::array<float, 12>& weights,
                             int suit_idx, int player_count) {
    float bonus = 0.0f;
    for (int d = 0; d < 12; ++d) {
        if (DECK_TABLE[d].goal_suit_index == suit_idx)
            bonus += weights[d] * static_cast<float>(DECK_TABLE[d].majority_bonus)
                   / static_cast<float>(std::max(1, player_count));
    }
    return bonus;
}

// Base EV per card for suit s: P(goal=s)*10 + expected bonus share.
inline float base_ev(const std::array<float, 12>& weights,
                      int suit_idx, int player_count) {
    auto P = goal_posteriors(weights);
    return P[suit_idx] * 10.0f + expected_bonus(weights, suit_idx, player_count);
}

// Renormalise deck_weights to sum to 1. Falls back to uniform on zero-sum.
inline void renormalise(std::array<float, 12>& weights) {
    float sum = 0.0f;
    for (float w : weights) sum += w;
    if (sum > 0.0f)
        for (float& w : weights) w /= sum;
    else
        weights.fill(1.0f / 12.0f);
}

// ── Bot config ────────────────────────────────────────────────────────────────

// Per-difficulty parameters — constructed from ServerConfig::BotsConfig by BotManager.
struct BotConfig {
    int   deck_multiplier     = 1;           // card scale for hard-mode games (1 = standard 40-card deck)
    float confidence_discount;    // Easy=0.80, Medium=0.90, Hard=1.00
    float taker_threshold;        // Easy=0.85, Medium=0.95, Hard=1.05
    float min_bid_ev;             // Easy=3.0,  Medium=2.0,  Hard=1.5
    float max_ask_ev;             // Easy=7.0,  Medium=8.0,  Hard=8.5
    int   hand_size_cap;          // Easy=4,    Medium=5,    Hard=6
    int   offload_threshold;      // Easy=3,    Medium=4,    Hard=4
    int   max_concurrent_orders;  // Easy=2,    Medium=4,    Hard=6
    float conviction_threshold;   // Medium=0.70, Hard=0.85; Easy: unused
    int   max_resting_ms;         // Easy=6000, Medium=3000, Hard=1500
    float nudge_probability;      // Easy=0.60, Medium=0.40, Hard=0.10
    int   nudge_patience_ms;      // Easy=3000, Medium=1500, Hard=500
    int   nudge_max_gap;          // Easy=3,    Medium=2,    Hard=1
    int   endgame_threshold_s;    // Easy=0,    Medium=15,   Hard=45
    float early_seed_threshold;   // Hard=0.40; Easy/Medium: set to 0
    float quoting_kappa;          // Easy=0.30, Medium=0.60, Hard=1.00
};

// ── Pending orders ────────────────────────────────────────────────────────────

struct BotPendingOrder {
    std::string order_id;
    Suit        suit;
    Side        side;
    int32_t     price = 0;
    std::chrono::steady_clock::time_point placed_at;
};

// ── Actions ───────────────────────────────────────────────────────────────────

struct BotSubmitOrder { Suit suit; Side side; int32_t price; int32_t qty = 1; };
struct BotCancelOrder { std::string order_id; };
struct BotNoAction    {};
using  BotAction = std::variant<BotSubmitOrder, BotCancelOrder, BotNoAction>;

// ── Events ────────────────────────────────────────────────────────────────────

struct BotRoundStartEvent {
    std::array<int, 4> hand;
    float   round_duration_s;
    int     player_slot;
    int     player_count;
    int32_t balance;
    int32_t buy_in;
    int32_t points_per_card;
};
struct BotBookUpdateEvent {
    Suit suit;
    std::optional<int32_t> best_bid;
    std::optional<int32_t> best_ask;
    std::optional<int32_t> best_bid_qty;
    std::optional<int32_t> best_ask_qty;
};
struct BotTradeEvent {
    Suit    suit;
    int32_t price;
    std::optional<Side> your_side;
};
struct BotDeltaUpdateEvent {
    std::vector<std::array<int, 4>> delta_table;
};
struct BotOrderAckEvent {
    std::string order_id;
    Suit    suit;
    Side    side;
    int32_t price;
};
struct BotRoundEndEvent {
    Suit    goal_suit;
    int32_t final_balance;
};
struct BotInterRoundEvent {};
struct BotGameEndedEvent  {};

using BotEvent = std::variant<
    BotRoundStartEvent,
    BotBookUpdateEvent,
    BotTradeEvent,
    BotDeltaUpdateEvent,
    BotOrderAckEvent,
    BotRoundEndEvent,
    BotInterRoundEvent,
    BotGameEndedEvent
>;

} // namespace anjeer::engine
