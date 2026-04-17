#pragma once
#include "engine/suit.h"
#include "engine/game_state.h"
#include <vector>

namespace anjeer::engine {

struct ScoringConfig {
    int buy_in;
    int points_per_card;
};

struct PlayerResult {
    int  player_slot;
    int  goal_cards_held;
    int  payout;
    bool disconnected;
};

struct RoundResult {
    Suit                      goal_suit;
    int                       pot;
    int                       bonus_pool;
    std::vector<PlayerResult> player_results;
};

// Pure function, no I/O. Returns payouts only — the server owns all balance mutations.
//
// Majority bonus rule:
//   threshold = total_goal_cards / 2 + 1  (strict majority)
//   Exactly one player with >= threshold → receives full bonus_pool.
//   Otherwise → bonus split evenly among players holding the most goal cards.
//
// Bonus split uses integer division — leftover coins are dropped.
// total_goal_cards is derived from hands (sum of goal-suit counts across all players).
// Precondition: hands.size() == disconnected.size().
//
// [[nodiscard]]: caller must dispatch or log the result.
[[nodiscard]] RoundResult score_round(
    const std::vector<PlayerHand>& hands,
    Suit                           goal_suit,
    const std::vector<bool>&       disconnected,
    const ScoringConfig&           cfg
);

} // namespace anjeer::engine
