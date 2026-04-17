#include "engine/scoring_engine.h"
#include <algorithm>
#include <cassert>

namespace anjeer::engine {

RoundResult score_round(
    const std::vector<PlayerHand>& hands,
    Suit                           goal_suit,
    const std::vector<bool>&       disconnected,
    const ScoringConfig&           cfg)
{
    assert(hands.size() == disconnected.size());

    const int N      = static_cast<int>(hands.size());
    const int goal_si = suit_index(goal_suit);

    // Compute total goal-suit cards across all hands.
    int total_goal_cards = 0;
    for (const auto& h : hands)
        total_goal_cards += h.suit_counts[goal_si];

    const int pot        = N * cfg.buy_in;
    const int bonus_pool = pot - (cfg.points_per_card * total_goal_cards);

    // AGENT-CTX: threshold uses total cards held at round end, not the original
    // deal distribution. In a fair game these are the same (cards are conserved
    // via transfer_card), but computing from hands makes score_round self-contained
    // and correct even if the server ever calls it mid-round for preview purposes.
    const int threshold = total_goal_cards / 2 + 1;

    // Find majority holder: exactly one player with >= threshold.
    int majority_holder = -1;
    int majority_count  = 0;
    for (int i = 0; i < N; ++i) {
        if (hands[i].suit_counts[goal_si] >= threshold) {
            majority_holder = i;
            majority_count++;
        }
    }

    // Compute per-player bonus allocation.
    std::vector<int> bonus(N, 0);
    if (majority_count == 1) {
        bonus[majority_holder] = bonus_pool;
    } else {
        // Plurality: split among all players holding the maximum count.
        int max_held = 0;
        for (int i = 0; i < N; ++i)
            max_held = std::max(max_held, hands[i].suit_counts[goal_si]);

        int plurality_count = 0;
        for (int i = 0; i < N; ++i)
            if (hands[i].suit_counts[goal_si] == max_held) plurality_count++;

        const int per = bonus_pool / plurality_count;
        for (int i = 0; i < N; ++i)
            if (hands[i].suit_counts[goal_si] == max_held) bonus[i] = per;
    }

    RoundResult result;
    result.goal_suit   = goal_suit;
    result.pot         = pot;
    result.bonus_pool  = bonus_pool;
    result.player_results.reserve(N);

    for (int i = 0; i < N; ++i) {
        const int goal_cards = hands[i].suit_counts[goal_si];
        const int payout     = goal_cards * cfg.points_per_card + bonus[i];
        result.player_results.push_back(PlayerResult{
            i,
            goal_cards,
            payout,
            disconnected[i],
        });
    }

    return result;
}

} // namespace anjeer::engine
