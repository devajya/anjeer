#pragma once
#include "engine/suit.h"
#include <array>
#include <random>
#include <vector>

namespace anjeer::engine {

// ---------------------------------------------------------------------------
// PlayerHand — per-player card counts by suit.
//
// suit_counts[suit_index(s)] = number of cards of suit s held by this player.
// Cards are fungible within a suit; only counts matter for scoring.
//
// has_extra_card is set on the one player who receives the remainder card in an
// uneven deal (e.g. 3 players × 40 cards → one player gets 14 instead of 13).
// ---------------------------------------------------------------------------
struct PlayerHand {
    std::array<int, 4> suit_counts{};  // zero-initialized; fixed size 4 (one per suit)
    bool has_extra_card{false};
};

// ---------------------------------------------------------------------------
// DealResult — output of GameState::deal().
//
// Contains everything the server needs to broadcast round_starting and send
// per-player round_start messages. Server owns serialization; engine does not.
// ---------------------------------------------------------------------------
struct DealResult {
    // Total cards assigned to each suit, indexed by suit_index().
    // Invariant: sum == GameState::Config::total_cards.
    // Invariant: the distribution is a permutation of Config::card_distribution.
    // Not sent to clients — server uses this to verify deal invariants.
    std::array<int, 4> suit_totals{};

    // The goal suit: color_partner of the suit that received the most cards.
    // Withheld from all wire messages until round end.
    Suit goal_suit{};

    // Per-player hands. Size == GameState::Config::player_count.
    std::vector<PlayerHand> hands;

    // True when Config::total_cards % Config::player_count != 0.
    // Exactly one hand will have has_extra_card == true when this is true.
    bool uneven_deal{false};
};

// ---------------------------------------------------------------------------
// GameState — owns deck deal, goal-suit derivation, and per-player hand state for one round.
// Order books live in the server layer until Slice 6 reunites them here under GameSession.
//
// Lifecycle:
//   1. Construct once per round.
//   2. Call deal() exactly once. Hands and goal_suit become valid.
//   3. Use hand(p) / goal_suit() as the round progresses.
//   4. Discard the GameState at round end; construct a new one for the next round.
// ---------------------------------------------------------------------------
class GameState {
public:
    struct Config {
        int player_count;
        int total_cards;                       // sum(card_distribution) must equal this
        std::array<int, 4> card_distribution;  // shuffled and assigned to suits in deal()
    };

    explicit GameState(Config cfg);

    // Shuffle and deal. Returns DealResult. Mutates internal hand and goal-suit state.
    // Must be called exactly once per GameState instance. Re-dealing belongs in a new
    // GameState instance (one instance per round).
    //
    // rng is caller-supplied (not stored) so unit tests can seed it deterministically
    // for reproducible hands.
    //
    // [[nodiscard]]: DealResult carries per-player hands the server must dispatch.
    [[nodiscard]] DealResult deal(std::mt19937& rng);

    // Per-player hand access. Valid only after deal().
    // player_slot is [0, player_count).
    [[nodiscard]] const PlayerHand& hand(int player_slot) const;

    // Goal suit. Valid only after deal().
    [[nodiscard]] Suit goal_suit() const;

    [[nodiscard]] int player_count() const;

    // Transfer one card of `suit` from player `from_slot` to player `to_slot`.
    // Called by the server after every executed trade to keep hand state current
    // for scoring at round end.
    //
    // AGENT-CTX: This is the only mutating method after deal() by design. The
    // server is the authoritative source of who bought/sold what (via TradeEvent),
    // so it drives all transfers. Engine does not observe order books directly
    // until GameSession wraps them in Slice 6.
    //
    // Precondition: hand(from_slot).suit_counts[suit_index(suit)] >= 1.
    // Asserts in debug; caller must ensure validity.
    void transfer_card(int from_slot, int to_slot, Suit suit);

    // Rule: color_partner of the suit with the highest card count.
    // Static so unit tests can call it without constructing a full GameState.
    [[nodiscard]] static Suit derive_goal_suit(const std::array<int, 4>& suit_totals);

private:
    Config cfg_;

    std::vector<PlayerHand> hands_;  // runtime-sized; capacity reserved in constructor

    Suit goal_suit_{};

    bool dealt_{false};  // guards against double-deal and pre-deal access
};

} // namespace anjeer::engine
