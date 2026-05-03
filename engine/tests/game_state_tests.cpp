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
// Uses Deck 1: clubs=10, diamonds=8, hearts=10, spades=12, goal=Clubs.
static GameState::Config make_test_config(int player_count = 5,
                                          Suit goal = Suit::Clubs) {
    return GameState::Config{
        .player_count      = player_count,
        .total_cards       = 40,
        .card_distribution = {10, 8, 10, 12},
        .goal_suit         = goal,
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

TEST_CASE("deal_returns_configured_goal_suit", "[game_state][deal][goal_suit]") {
    // goal_suit is explicit in the deck config; deal() must return it unchanged.
    for (auto goal : {Suit::Clubs, Suit::Diamonds, Suit::Hearts, Suit::Spades}) {
        GameState gs(make_test_config(5, goal));
        std::mt19937 rng(42);
        auto result = gs.deal(rng);
        REQUIRE(result.goal_suit == goal);
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

// ═══════════════════════════════════════════════════════════════════════════
// transfer_card tests — Slice 4 bug fix
//
// AGENT-CTX: transfer_card() is called by the server after every executed
// trade so that hand state stays accurate for end-of-round scoring. These
// tests verify the mutation is correct and isolated to the transferred suit.
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("transfer_card: reduces sender count and increases receiver count", "[game_state][transfer]") {
    GameState gs(make_test_config(5));
    std::mt19937 rng(42);
    auto deal = gs.deal(rng);

    // Find the first suit where player 0 holds at least one card.
    Suit transfer_suit = Suit::Clubs;
    for (auto s : kAllSuits) {
        if (deal.hands[0].suit_counts[suit_index(s)] > 0) {
            transfer_suit = s;
            break;
        }
    }

    const int si = suit_index(transfer_suit);
    const int from_before = gs.hand(0).suit_counts[si];
    const int to_before   = gs.hand(1).suit_counts[si];

    gs.transfer_card(0, 1, transfer_suit);

    REQUIRE(gs.hand(0).suit_counts[si] == from_before - 1);
    REQUIRE(gs.hand(1).suit_counts[si] == to_before + 1);
}

TEST_CASE("transfer_card: only the transferred suit count changes", "[game_state][transfer]") {
    // AGENT-CTX: Verifies the operation is surgical — other suits and other
    // players' hands are untouched. Critical invariant for scoring correctness.
    GameState gs(make_test_config(5));
    std::mt19937 rng(42);
    auto deal = gs.deal(rng);

    Suit transfer_suit = Suit::Clubs;
    for (auto s : kAllSuits) {
        if (deal.hands[0].suit_counts[suit_index(s)] > 0) {
            transfer_suit = s;
            break;
        }
    }

    // Capture complete hand state before transfer.
    const auto hand0_before = gs.hand(0);
    const auto hand1_before = gs.hand(1);
    const auto hand2_before = gs.hand(2);

    gs.transfer_card(0, 1, transfer_suit);

    // Other suits for both transacting players must be unchanged.
    for (auto s : kAllSuits) {
        const int si = suit_index(s);
        if (s == transfer_suit) continue;
        CHECK(gs.hand(0).suit_counts[si] == hand0_before.suit_counts[si]);
        CHECK(gs.hand(1).suit_counts[si] == hand1_before.suit_counts[si]);
    }
    // Non-transacting player untouched entirely.
    for (int si = 0; si < 4; ++si)
        CHECK(gs.hand(2).suit_counts[si] == hand2_before.suit_counts[si]);
}

TEST_CASE("transfer_card: total cards across all players is conserved", "[game_state][transfer]") {
    // AGENT-CTX: Conservation invariant — cards cannot be created or destroyed,
    // only moved. Failure here means a double-credit or double-debit bug.
    GameState gs(make_test_config(5));
    std::mt19937 rng(42);
    gs.deal(rng);

    // Count total cards before.
    auto total = [&]() {
        int t = 0;
        for (int p = 0; p < 5; ++p)
            for (int c : gs.hand(p).suit_counts) t += c;
        return t;
    };
    const int before = total();

    // Perform several transfers.
    Suit s0 = Suit::Clubs, s1 = Suit::Diamonds;
    // Ensure slot 0 has clubs and slot 1 has diamonds; fall back if not.
    for (auto s : kAllSuits) {
        if (gs.hand(0).suit_counts[suit_index(s)] > 0) { s0 = s; break; }
    }
    for (auto s : kAllSuits) {
        if (gs.hand(1).suit_counts[suit_index(s)] > 0) { s1 = s; break; }
    }
    gs.transfer_card(0, 1, s0);
    gs.transfer_card(1, 2, s1);

    REQUIRE(total() == before);
}
