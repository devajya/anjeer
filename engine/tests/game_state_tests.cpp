#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include "engine/engine.h"

// AGENT-CTX: These tests are the authoritative expression of Slice 3 acceptance
// criteria for the engine. Every AC in slice_definitions maps to at least one
// TEST_CASE below. Tests were written BEFORE full implementation (TDD / red-green):
//   - Suit helper tests (suit_index, same_color, color_partner, suit_name): PASS
//     immediately — suit.h is fully constexpr, no stub.
//   - All GameState deal tests: FAIL (RED) against the T3 stub; go GREEN in T6.
// Do not remove a test without removing the corresponding AC.

using namespace anjeer::engine;

// ---------------------------------------------------------------------------
// Test config helper
// AGENT-CTX: Centralises default test values so tests are insulated from config
// field additions. player_count is parameterised; all other values match the
// production default.json (40 cards, {12,10,10,8} distribution, price 1–99).
// Tests that need a different count call make_test_config(N) explicitly.
// ---------------------------------------------------------------------------
static GameState::Config make_test_config(int player_count = 5) {
    return GameState::Config{
        .player_count      = player_count,
        .total_cards       = 40,
        .card_distribution = {12, 10, 10, 8},
    };
}

// ═══════════════════════════════════════════════════════════════════════════
// Suit helper tests — fully implemented in suit.h (constexpr). All PASS now.
// AGENT-CTX: These invariants are depended on by derive_goal_suit and the
// server's suit routing. Keeping them here (not in a separate file) makes it
// clear they are prerequisites for GameState correctness.
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("suit_index_is_stable", "[suit]") {
    // AGENT-CTX: All per-suit std::array<T,4> in GameState are indexed by
    // suit_index(). Verifying the stable mapping guards against silent
    // misrouting if the enum declaration order is ever changed.
    REQUIRE(suit_index(Suit::Clubs)    == 0);
    REQUIRE(suit_index(Suit::Diamonds) == 1);
    REQUIRE(suit_index(Suit::Hearts)   == 2);
    REQUIRE(suit_index(Suit::Spades)   == 3);
}

TEST_CASE("same_color_all_pairs", "[suit]") {
    // Black pairs
    REQUIRE( same_color(Suit::Clubs,    Suit::Spades));
    REQUIRE( same_color(Suit::Spades,   Suit::Clubs));
    // Red pairs
    REQUIRE( same_color(Suit::Hearts,   Suit::Diamonds));
    REQUIRE( same_color(Suit::Diamonds, Suit::Hearts));
    // Cross-color pairs (all 8 orderings)
    REQUIRE(!same_color(Suit::Clubs,    Suit::Hearts));
    REQUIRE(!same_color(Suit::Clubs,    Suit::Diamonds));
    REQUIRE(!same_color(Suit::Spades,   Suit::Hearts));
    REQUIRE(!same_color(Suit::Spades,   Suit::Diamonds));
    REQUIRE(!same_color(Suit::Hearts,   Suit::Clubs));
    REQUIRE(!same_color(Suit::Hearts,   Suit::Spades));
    REQUIRE(!same_color(Suit::Diamonds, Suit::Clubs));
    REQUIRE(!same_color(Suit::Diamonds, Suit::Spades));
}

TEST_CASE("color_partner_is_symmetric", "[suit]") {
    // AGENT-CTX: color_partner(color_partner(s)) == s is an invariant that
    // derive_goal_suit relies on. Verifying it here catches future breakage.
    REQUIRE(color_partner(Suit::Clubs)    == Suit::Spades);
    REQUIRE(color_partner(Suit::Spades)   == Suit::Clubs);
    REQUIRE(color_partner(Suit::Hearts)   == Suit::Diamonds);
    REQUIRE(color_partner(Suit::Diamonds) == Suit::Hearts);

    for (auto s : {Suit::Clubs, Suit::Diamonds, Suit::Hearts, Suit::Spades}) {
        REQUIRE(color_partner(color_partner(s)) == s);
    }
}

TEST_CASE("suit_name_matches_expected_strings", "[suit]") {
    // AGENT-CTX: Hardcoded expected strings (not derived from suit_name) so a
    // rename surfaces as a test failure. These strings must stay in sync with
    // config/default.json "order_book.active_suits" and messages.ts RoundStartMessage.
    REQUIRE(suit_name(Suit::Clubs)    == "clubs");
    REQUIRE(suit_name(Suit::Diamonds) == "diamonds");
    REQUIRE(suit_name(Suit::Hearts)   == "hearts");
    REQUIRE(suit_name(Suit::Spades)   == "spades");
}

// ═══════════════════════════════════════════════════════════════════════════
// derive_goal_suit — static, testable without a full GameState (all FAIL RED)
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("goal_suit_all_four_assignments", "[game_state][goal_suit]") {
    // AC4: For each of the 4 possible suits holding 12 cards, derive_goal_suit
    // must return color_partner of that suit.
    // AGENT-CTX: Tests the static method directly so the goal-suit rule can be
    // verified without going through a full shuffle/deal cycle. Each case
    // places 12 cards on one suit (clearly the max) and uses small values for
    // the others — derive_goal_suit only needs to find the maximum, not sum.
    struct Case {
        std::array<int, 4> totals;
        Suit expected_goal;
    };

    const std::array<Case, 4> cases{{
        // Clubs=12 → goal=Spades (Black partner)
        { {12, 10,  8, 10}, color_partner(Suit::Clubs)    },
        // Diamonds=12 → goal=Hearts (Red partner)
        { {10, 12, 10,  8}, color_partner(Suit::Diamonds) },
        // Hearts=12 → goal=Diamonds (Red partner)
        { { 8, 10, 12, 10}, color_partner(Suit::Hearts)   },
        // Spades=12 → goal=Clubs (Black partner)
        { {10,  8, 10, 12}, color_partner(Suit::Spades)   },
    }};

    for (const auto& c : cases) {
        REQUIRE(GameState::derive_goal_suit(c.totals) == c.expected_goal);
        // Also verify the color invariant holds
        Suit max_suit = static_cast<Suit>(
            static_cast<int>(std::distance(c.totals.begin(),
                std::max_element(c.totals.begin(), c.totals.end())))
        );
        REQUIRE(same_color(max_suit, c.expected_goal));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Full deal() tests (all FAIL RED against stub — go GREEN in T6)
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("deal_total_cards_conserved", "[game_state][deal]") {
    // AC1: Sum of all suit_counts across all player hands == total_cards (40).
    GameState gs(make_test_config(5));
    std::mt19937 rng(42);
    auto result = gs.deal(rng);

    int total = 0;
    for (const auto& h : result.hands)
        for (int c : h.suit_counts) total += c;
    REQUIRE(total == 40);
}

TEST_CASE("deal_suit_totals_are_valid_distribution", "[game_state][deal]") {
    // AC2: The sorted suit_totals must equal the sorted card_distribution.
    // i.e. {8,10,10,12} regardless of which suit got which count.
    GameState gs(make_test_config(5));
    std::mt19937 rng(42);
    auto result = gs.deal(rng);

    auto totals = result.suit_totals;
    std::sort(totals.begin(), totals.end());
    REQUIRE(totals == (std::array<int, 4>{8, 10, 10, 12}));
}

TEST_CASE("goal_suit_is_color_partner_of_twelve_suit", "[game_state][deal][goal_suit]") {
    // AC3: result.goal_suit == color_partner of the suit that received 12 cards.
    // Verified over 20 seeds so we cover multiple shuffle outcomes.
    for (int seed = 0; seed < 20; ++seed) {
        GameState gs(make_test_config(5));
        std::mt19937 rng(seed);
        auto result = gs.deal(rng);

        // Confirm a suit really got 12 cards
        auto max_it = std::max_element(result.suit_totals.begin(), result.suit_totals.end());
        REQUIRE(*max_it == 12);

        int max_si = static_cast<int>(
            std::distance(result.suit_totals.begin(), max_it));
        Suit twelve_suit = static_cast<Suit>(max_si);

        REQUIRE(result.goal_suit == color_partner(twelve_suit));
        REQUIRE(same_color(twelve_suit, result.goal_suit));
    }
}

TEST_CASE("deal_hands_have_correct_sizes_even", "[game_state][deal]") {
    // AC5 (even case): 40 cards / 5 players → each player gets exactly 8.
    GameState gs(make_test_config(5));
    std::mt19937 rng(42);
    auto result = gs.deal(rng);

    for (const auto& h : result.hands) {
        int hand_size = 0;
        for (int c : h.suit_counts) hand_size += c;
        REQUIRE(hand_size == 8);
    }
}

TEST_CASE("deal_hands_have_correct_sizes_uneven", "[game_state][deal]") {
    // AC5 (uneven case): 40 cards / 3 players.
    // AGENT-CTX: Per Slice 3 resolution 4, ALL extra cards go to ONE player.
    // base = 40/3 = 13, remainder = 1.
    // So: N-1=2 players get 13 cards, 1 player gets 14 (13+1).
    GameState gs(make_test_config(3));
    std::mt19937 rng(42);
    auto result = gs.deal(rng);

    int base = 40 / 3;    // 13
    int remainder = 40 % 3; // 1
    int players_with_base  = 0;
    int players_with_extra = 0;

    for (const auto& h : result.hands) {
        int hand_size = 0;
        for (int c : h.suit_counts) hand_size += c;
        if (hand_size == base)          players_with_base++;
        if (hand_size == base + remainder) players_with_extra++;
    }

    REQUIRE(players_with_extra == 1);
    REQUIRE(players_with_base  == 2);
}

TEST_CASE("uneven_deal_flag_set_for_three_players", "[game_state][deal][uneven]") {
    // AC6: uneven_deal=true when total_cards % player_count != 0.
    GameState gs(make_test_config(3));
    std::mt19937 rng(42);
    auto result = gs.deal(rng);
    REQUIRE(result.uneven_deal);
}

TEST_CASE("exactly_one_player_has_extra_card", "[game_state][deal][uneven]") {
    // AC7: When uneven_deal=true, exactly one player has has_extra_card=true.
    // AGENT-CTX: has_extra_card marks the player with informational edge for
    // future EV calculations. Must be set on exactly one player — not zero, not two.
    GameState gs(make_test_config(3));
    std::mt19937 rng(1234);
    auto result = gs.deal(rng);

    REQUIRE(result.uneven_deal);
    int extra_count = 0;
    for (const auto& h : result.hands) {
        if (h.has_extra_card) extra_count++;
    }
    REQUIRE(extra_count == 1);
}

TEST_CASE("no_extra_card_on_even_deal", "[game_state][deal]") {
    // AC8: When total_cards % player_count == 0, no player has has_extra_card,
    // and uneven_deal is false.
    GameState gs(make_test_config(5));
    std::mt19937 rng(42);
    auto result = gs.deal(rng);

    REQUIRE(!result.uneven_deal);
    for (const auto& h : result.hands) {
        REQUIRE(!h.has_extra_card);
    }
}

TEST_CASE("deal_randomizes_suit_assignment", "[game_state][deal]") {
    // AC10: Over many seeds, each of the 4 suits must appear as the 12-card suit
    // at least once. Guards against a degenerate stub that always assigns 12 to Clubs.
    // AGENT-CTX: std::shuffle on a 4-element array with a seed-varied rng produces
    // all 4! = 24 permutations across 100 seeds with overwhelming probability.
    std::array<bool, 4> max_suit_seen{};

    for (int seed = 0; seed < 100; ++seed) {
        GameState gs(make_test_config(5));
        std::mt19937 rng(seed);
        auto result = gs.deal(rng);

        auto max_it = std::max_element(result.suit_totals.begin(), result.suit_totals.end());
        int max_si = static_cast<int>(
            std::distance(result.suit_totals.begin(), max_it));
        max_suit_seen[max_si] = true;
    }

    REQUIRE(std::all_of(max_suit_seen.begin(), max_suit_seen.end(),
                        [](bool b) { return b; }));
}

TEST_CASE("hand_suit_counts_sum_to_hand_size", "[game_state][deal]") {
    // AC11: For each player, sum of suit_counts equals their expected hand size.
    // Tests that cards are fully accounted for within each hand (no phantom suit).
    GameState gs(make_test_config(5));
    std::mt19937 rng(7);
    auto result = gs.deal(rng);

    const int expected_per_player = 40 / 5; // 8
    for (int p = 0; p < 5; ++p) {
        int sum = 0;
        for (int c : result.hands[p].suit_counts) sum += c;
        REQUIRE(sum == expected_per_player);
    }
}

TEST_CASE("deal_total_cards_conservation_across_all_players", "[game_state][deal]") {
    // AC1 (cross-player): Sum of ALL suit_counts across ALL players equals total_cards.
    // Different from deal_total_cards_conserved only in structure — verifies the
    // same cards are not counted in multiple players' hands (double-dealing bug).
    // AGENT-CTX: Run with N=3 (uneven) to also cover the extra-card path.
    GameState gs(make_test_config(3));
    std::mt19937 rng(99);
    auto result = gs.deal(rng);

    int grand_total = 0;
    for (const auto& h : result.hands)
        for (int c : h.suit_counts) grand_total += c;
    REQUIRE(grand_total == 40);
}
