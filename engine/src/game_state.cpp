#include "engine/game_state.h"
#include <algorithm>
#include <cassert>
#include <random>

namespace anjeer::engine {

// ---------------------------------------------------------------------------
// GameState — constructor
// ---------------------------------------------------------------------------

GameState::GameState(Config cfg)
    : cfg_(std::move(cfg))
{
    // AGENT-CTX: Reserve capacity now so deal() can use assign without any
    // reallocation. player_count is known at construction time.
    hands_.reserve(cfg_.player_count);
}

// ---------------------------------------------------------------------------
// GameState::deal — shuffle and distribute cards
// ---------------------------------------------------------------------------

DealResult GameState::deal(std::mt19937& rng) {
    assert(!dealt_ && "deal() called twice on the same GameState instance");
    dealt_ = true;

    // ── Step 1: Randomly assign card counts to suits ──────────────────────
    //
    // AGENT-CTX: cfg_.card_distribution is {12,10,10,8} in the default config
    // but is treated as an unordered multiset here. std::shuffle maps each
    // element to a random suit index. After shuffle, distribution[suit_index(s)]
    // is the number of cards suit s receives this round.
    std::array<int, 4> distribution = cfg_.card_distribution;
    std::shuffle(distribution.begin(), distribution.end(), rng);

    DealResult result;
    result.suit_totals = distribution;
    result.goal_suit   = derive_goal_suit(distribution);
    goal_suit_         = result.goal_suit;

    // ── Step 2: Build the deck as a flat vector of Suit values ────────────
    //
    // AGENT-CTX: Each element of deck[] represents one card. Since cards are
    // fungible within a suit (ranks deferred to future slice), the only
    // information encoded is which suit the card belongs to.
    // reserve() prevents reallocation — total_cards is known at this point.
    // emplace_back is preferred over push_back per cpp_performance_rules.md.
    std::vector<Suit> deck;
    deck.reserve(cfg_.total_cards);
    for (int si = 0; si < 4; ++si) {
        const Suit s = static_cast<Suit>(si);
        for (int i = 0; i < distribution[si]; ++i)
            deck.emplace_back(s);
    }

    // Shuffle the deck to randomise which player receives which suits.
    std::shuffle(deck.begin(), deck.end(), rng);

    // ── Step 3: Compute hand sizes ────────────────────────────────────────
    //
    // AGENT-CTX: Per Slice 3 resolution 4, ALL extra cards go to exactly ONE
    // randomly chosen player. Even if remainder > 1 (e.g. N=6, 40 cards →
    // remainder=4), one player absorbs all 4 extra cards. This keeps
    // has_extra_card a single-player flag and simplifies future EV analysis.
    // The alternative (distributing one extra card each to `remainder` players)
    // was explicitly rejected in the resolution ("give any ONE player an extra
    // card"). If this policy changes, update this block and the tests for
    // "exactly_one_player_has_extra_card".
    const int N         = cfg_.player_count;
    const int base      = cfg_.total_cards / N;
    const int remainder = cfg_.total_cards % N;

    result.uneven_deal = (remainder != 0);

    // AGENT-CTX: extra_player is -1 for even deals (no extra card to give).
    // For uneven deals, the extra player is chosen uniformly at random so
    // no player slot is systematically advantaged across rounds.
    int extra_player = -1;
    if (remainder > 0) {
        std::uniform_int_distribution<int> pick(0, N - 1);
        extra_player = pick(rng);
    }

    // ── Step 4: Deal cards to players ─────────────────────────────────────
    //
    // AGENT-CTX: Strategy: give each player `base` cards from the front of
    // the shuffled deck, then give the remaining `remainder` cards to
    // extra_player. This preserves the randomness from the deck shuffle —
    // no secondary shuffle of player order is needed.
    //
    // DealResult::hands and hands_ are kept in sync so both the result
    // (returned to the server for dispatch) and the internal state (accessed
    // via hand()) are consistent. The duplication is intentional: the server
    // needs a value-type snapshot to serialise; internal state is for query.
    // AGENT-CTX (future): When GameSession wraps GameState in Slice 6, the
    // internal hands_ and the DealResult might be unified. Until then, keep
    // both. The copy is O(N × 4 ints) — negligible.
    hands_.assign(N, PlayerHand{});
    result.hands.assign(N, PlayerHand{});

    int card_idx = 0;

    // Deal base cards to every player (round-robin over players, sequential deck)
    for (int p = 0; p < N; ++p) {
        for (int c = 0; c < base; ++c) {
            const int si = suit_index(deck[card_idx++]);
            hands_[p].suit_counts[si]++;
            result.hands[p].suit_counts[si]++;
        }
    }

    // Deal remainder cards to extra_player (remainder == 0 → loop doesn't run)
    for (int c = 0; c < remainder; ++c) {
        const int si = suit_index(deck[card_idx++]);
        hands_[extra_player].suit_counts[si]++;
        result.hands[extra_player].suit_counts[si]++;
    }

    // Mark the extra-card player
    // AGENT-CTX: has_extra_card is the signal for the server to log the
    // informational edge. Grep `has_extra_card` to find the server-side log.
    // This flag intentionally does nothing else in Slice 3 — it is a seam
    // for future EV calculations.
    if (extra_player >= 0) {
        hands_[extra_player].has_extra_card        = true;
        result.hands[extra_player].has_extra_card  = true;
    }

    // Sanity check: all cards in deck were consumed
    // AGENT-CTX: assert (not throw) — reaching this with card_idx != total_cards
    // is always a programming error in the deal logic, not a runtime condition.
    assert(card_idx == cfg_.total_cards);

    return result;
}

// ---------------------------------------------------------------------------
// GameState — accessors
// ---------------------------------------------------------------------------

const PlayerHand& GameState::hand(int player_slot) const {
    assert(dealt_ && "hand() called before deal()");
    assert(player_slot >= 0 && player_slot < static_cast<int>(hands_.size()));
    return hands_[static_cast<std::size_t>(player_slot)];
}

Suit GameState::goal_suit() const {
    assert(dealt_ && "goal_suit() called before deal()");
    return goal_suit_;
}

int GameState::player_count() const {
    return cfg_.player_count;
}

// ---------------------------------------------------------------------------
// GameState::transfer_card — post-trade hand mutation
// ---------------------------------------------------------------------------

void GameState::transfer_card(int from_slot, int to_slot, Suit suit) {
    assert(dealt_ && "transfer_card() called before deal()");
    assert(from_slot >= 0 && from_slot < static_cast<int>(hands_.size()));
    assert(to_slot   >= 0 && to_slot   < static_cast<int>(hands_.size()));
    const int si = suit_index(suit);
    // AGENT-CTX: Assert (not return/throw) — a transfer with an empty source hand
    // means the engine and server book are out of sync, which is always a
    // programming error, not a recoverable runtime condition.
    assert(hands_[from_slot].suit_counts[si] > 0 &&
           "transfer_card(): from_slot has no cards of this suit");
    hands_[from_slot].suit_counts[si]--;
    hands_[to_slot].suit_counts[si]++;
}

// ---------------------------------------------------------------------------
// GameState::derive_goal_suit — static, pure function
// ---------------------------------------------------------------------------

Suit GameState::derive_goal_suit(const std::array<int, 4>& suit_totals) {
    // AGENT-CTX: Rule: goal = color_partner(suit with the maximum card count).
    // For the standard distribution {12,10,10,8}, the max is always 12 and is
    // unique. std::max_element returns the FIRST maximum in case of ties — this
    // is deterministic but ties are not expected with the current distribution.
    // If the card_distribution config ever allows ties for the maximum (e.g.
    // {12,12,8,8}), the goal suit would be determined by iteration order (i.e.
    // whichever suit with 12 appears first in the Suit enum). Document that
    // constraint in the config if tie-distributions are ever considered.
    //
    // AGENT-CTX: std::max_element on a 4-element fixed-size array is O(4) and
    // inlined by the compiler. No performance concern at call frequency (once
    // per round start). std::distance on random-access iterators is O(1).
    auto max_it = std::max_element(suit_totals.begin(), suit_totals.end());
    const int max_si = static_cast<int>(
        std::distance(suit_totals.begin(), max_it));
    return color_partner(static_cast<Suit>(max_si));
}

} // namespace anjeer::engine
