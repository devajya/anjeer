#include <catch2/catch_test_macros.hpp>
#include "engine/engine.h"

using namespace anjeer::engine;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static PlayerHand make_hand(int clubs, int diamonds, int hearts, int spades) {
    PlayerHand h;
    h.suit_counts[suit_index(Suit::Clubs)]    = clubs;
    h.suit_counts[suit_index(Suit::Diamonds)] = diamonds;
    h.suit_counts[suit_index(Suit::Hearts)]   = hearts;
    h.suit_counts[suit_index(Suit::Spades)]   = spades;
    return h;
}

// Default config matching config/default.json scoring section.
static ScoringConfig default_cfg() {
    return ScoringConfig{ .buy_in = 50, .points_per_card = 20 };
}

// ═══════════════════════════════════════════════════════════════════════════
// Structural invariants — pot and bonus_pool
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("score_round: pot = player_count × buy_in", "[scoring]") {
    // 5 players × buy_in=50 → pot=250
    std::vector<PlayerHand> hands(5, make_hand(2, 2, 2, 2));
    std::vector<int>  balances(5, 100);
    std::vector<bool> disc(5, false);

    auto r = score_round(hands, Suit::Spades, balances, disc, default_cfg());
    REQUIRE(r.pot == 250);
}

TEST_CASE("score_round: bonus_pool = pot - (points_per_card × total_goal_cards)", "[scoring]") {
    // 5 players each hold 2 spades → total_goal_cards=10
    // bonus_pool = 250 - (20 × 10) = 50
    std::vector<PlayerHand> hands(5, make_hand(2, 2, 2, 2));
    std::vector<int>  balances(5, 100);
    std::vector<bool> disc(5, false);

    auto r = score_round(hands, Suit::Spades, balances, disc, default_cfg());
    REQUIRE(r.bonus_pool == 50);
}

TEST_CASE("score_round: new_balance = balance_before - buy_in + payout", "[scoring]") {
    // Each player holds 2 spades → payout=50 (40 card + 10 split bonus)
    // new_balance = 100 - 50 + 50 = 100
    std::vector<PlayerHand> hands(5, make_hand(2, 2, 2, 2));
    std::vector<int>  balances(5, 100);
    std::vector<bool> disc(5, false);

    auto r = score_round(hands, Suit::Spades, balances, disc, default_cfg());
    for (const auto& pr : r.player_results)
        REQUIRE(pr.new_balance == 100);
}

// ═══════════════════════════════════════════════════════════════════════════
// Per-card payout and plurality bonus
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("score_round: basic per-card payout, even split of bonus", "[scoring]") {
    // 5 players each hold 2 spades → total=10, threshold=6
    // Nobody has strict majority → plurality=2 held by all 5 → bonus_per=10
    // payout = 2×20 + 10 = 50; new_balance = 100 - 50 + 50 = 100
    std::vector<PlayerHand> hands(5, make_hand(2, 2, 2, 2));
    std::vector<int>  balances(5, 100);
    std::vector<bool> disc(5, false);

    auto r = score_round(hands, Suit::Spades, balances, disc, default_cfg());
    REQUIRE(r.player_results.size() == 5);
    for (const auto& pr : r.player_results) {
        CHECK(pr.goal_cards_held == 2);
        CHECK(pr.payout          == 50);
        CHECK(pr.new_balance     == 100);
    }
}

TEST_CASE("score_round: majority holder gets full bonus_pool", "[scoring]") {
    // Player 0 holds 7 spades (>= threshold=6) → sole majority holder
    // bonus_pool = 250 - (20×10) = 50
    // player 0 payout = 7×20 + 50 = 190; others get only card payout
    std::vector<PlayerHand> hands = {
        make_hand(0, 0, 0, 7),  // player 0: majority
        make_hand(0, 0, 0, 1),
        make_hand(0, 0, 0, 1),
        make_hand(0, 0, 0, 1),
        make_hand(0, 0, 0, 0),
    };
    std::vector<int>  balances(5, 100);
    std::vector<bool> disc(5, false);

    auto r = score_round(hands, Suit::Spades, balances, disc, default_cfg());
    REQUIRE(r.player_results[0].payout == 190);
    CHECK(r.player_results[1].payout   == 20);
    CHECK(r.player_results[2].payout   == 20);
    CHECK(r.player_results[3].payout   == 20);
    CHECK(r.player_results[4].payout   == 0);
}

TEST_CASE("score_round: tie for plurality → bonus split evenly", "[scoring]") {
    // Players 0 and 1 hold 4 spades each — nobody hits threshold=6
    // bonus_pool = 250 - (20×10) = 50 → split 2 ways = 25 each
    std::vector<PlayerHand> hands = {
        make_hand(0, 0, 0, 4),
        make_hand(0, 0, 0, 4),
        make_hand(0, 0, 0, 1),
        make_hand(0, 0, 0, 1),
        make_hand(0, 0, 0, 0),
    };
    std::vector<int>  balances(5, 100);
    std::vector<bool> disc(5, false);

    auto r = score_round(hands, Suit::Spades, balances, disc, default_cfg());
    CHECK(r.player_results[0].payout == 4 * 20 + 25);  // 105
    CHECK(r.player_results[1].payout == 4 * 20 + 25);  // 105
    CHECK(r.player_results[2].payout == 1 * 20);        // 20
    CHECK(r.player_results[3].payout == 1 * 20);        // 20
    CHECK(r.player_results[4].payout == 0);
}

TEST_CASE("score_round: zero goal cards held → payout is zero (no bonus)", "[scoring]") {
    // Player 4 holds 0 spades and is not the plurality holder
    std::vector<PlayerHand> hands = {
        make_hand(0, 0, 0, 7),
        make_hand(0, 0, 0, 1),
        make_hand(0, 0, 0, 1),
        make_hand(0, 0, 0, 1),
        make_hand(0, 0, 0, 0),
    };
    std::vector<int>  balances(5, 100);
    std::vector<bool> disc(5, false);

    auto r = score_round(hands, Suit::Spades, balances, disc, default_cfg());
    REQUIRE(r.player_results[4].goal_cards_held == 0);
    REQUIRE(r.player_results[4].payout          == 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Disconnected player
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("score_round: disconnected player — payout computed, disconnected=true", "[scoring]") {
    // Player 2 is disconnected. Their goal cards still count for majority math.
    // Their payout is computed normally; the server decides whether to hold it.
    std::vector<PlayerHand> hands(5, make_hand(2, 2, 2, 2));
    std::vector<int>  balances(5, 100);
    std::vector<bool> disc = { false, false, true, false, false };

    auto r = score_round(hands, Suit::Spades, balances, disc, default_cfg());
    CHECK(r.player_results[2].disconnected    == true);
    CHECK(r.player_results[2].goal_cards_held == 2);
    CHECK(r.player_results[2].payout          >  0);
    // Connected players unaffected
    CHECK(r.player_results[0].disconnected == false);
}

// ═══════════════════════════════════════════════════════════════════════════
// Config scaling
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("score_round: respects custom buy_in and points_per_card", "[scoring]") {
    // buy_in=30, points_per_card=10
    // 5 players each hold 2 spades → total_goal=10, pot=150, bonus=150-100=50
    // threshold=6; nobody → plurality 5 → bonus_per=10
    // payout = 2×10 + 10 = 30; new_balance = 100 - 30 + 30 = 100
    ScoringConfig cfg{ .buy_in = 30, .points_per_card = 10 };
    std::vector<PlayerHand> hands(5, make_hand(2, 2, 2, 2));
    std::vector<int>  balances(5, 100);
    std::vector<bool> disc(5, false);

    auto r = score_round(hands, Suit::Spades, balances, disc, cfg);
    CHECK(r.pot       == 150);
    CHECK(r.bonus_pool == 50);
    for (const auto& pr : r.player_results) {
        CHECK(pr.payout      == 30);
        CHECK(pr.new_balance == 100);
    }
}
