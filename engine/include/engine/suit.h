#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

// Suit enum and pure helpers. Kept separate from game_state.h so ScoringEngine
// and EvalModule can include this without pulling in the full GameState.

namespace anjeer::engine {

// ---------------------------------------------------------------------------
// Suit enum
//
// Declaration order (0=Clubs, 1=Diamonds, 2=Hearts, 3=Spades) is stable and
// used as array indices throughout GameState. Do not reorder without updating
// every std::array<T,4> indexed by suit_index().
// ---------------------------------------------------------------------------
enum class Suit : uint8_t {
    Clubs    = 0,
    Diamonds = 1,
    Hearts   = 2,
    Spades   = 3,
};

// Returns the integer index [0,3] for use as an array subscript.
constexpr int suit_index(Suit s) noexcept {
    return static_cast<int>(s);
}

// Returns true iff a and b share the same card color (Black={Clubs,Spades}, Red={Hearts,Diamonds}).
constexpr bool same_color(Suit a, Suit b) noexcept {
    auto is_black = [](Suit s) noexcept {
        return s == Suit::Clubs || s == Suit::Spades;
    };
    return is_black(a) == is_black(b);
}

// Returns the unique suit that shares the same color as s.
// Clubs↔Spades (Black),  Hearts↔Diamonds (Red).
constexpr Suit color_partner(Suit s) noexcept {
    switch (s) {
        case Suit::Clubs:    return Suit::Spades;
        case Suit::Spades:   return Suit::Clubs;
        case Suit::Hearts:   return Suit::Diamonds;
        case Suit::Diamonds: return Suit::Hearts;
    }
    return Suit::Clubs; // unreachable; silences -Wreturn-type
}

// Returns the canonical lowercase string name for a suit.
// Returns string_view into a static literal — zero allocation.
constexpr std::string_view suit_name(Suit s) noexcept {
    switch (s) {
        case Suit::Clubs:    return "clubs";
        case Suit::Diamonds: return "diamonds";
        case Suit::Hearts:   return "hearts";
        case Suit::Spades:   return "spades";
    }
    return "unknown"; // unreachable
}

// All four suits in declaration order. Use for iteration wherever all suits are needed.
constexpr std::array<Suit, 4> kAllSuits = {
    Suit::Clubs, Suit::Diamonds, Suit::Hearts, Suit::Spades,
};

// Parse a lowercase suit name to a Suit value.
// Returns nullopt for any unrecognized string.
constexpr std::optional<Suit> suit_from_string(std::string_view name) noexcept {
    if (name == "clubs")    return Suit::Clubs;
    if (name == "diamonds") return Suit::Diamonds;
    if (name == "hearts")   return Suit::Hearts;
    if (name == "spades")   return Suit::Spades;
    return std::nullopt;
}

} // namespace anjeer::engine
