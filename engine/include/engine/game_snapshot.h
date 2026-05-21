#pragma once

#include "engine/suit.h"
#include "engine/order_book.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace anjeer::engine {

// Deck configuration as seen by the eval subsystem.
// Distinct from DeckDef (which carries bot-internal fields like majority_bonus).
struct DeckSpec {
    std::array<int, 4> counts;   // indexed by suit_index(): C=0, D=1, H=2, S=3
    Suit goal_suit;
};

struct TradeRecord {
    int32_t buyer_slot;
    int32_t seller_slot;
    int32_t price;
    Suit    suit;
    int64_t timestamp_ms;        // ms since round start
};

struct BookSnapshot {
    std::optional<int32_t> best_bid;
    std::optional<int32_t> best_ask;
    std::optional<int32_t> best_bid_slot;
    std::optional<int32_t> best_ask_slot;
};

// Read-only view of game state — used by BotAgent::decide() each tick
// (via BotAdapter) and by EvalRunner snapshots at round boundaries.
// Bot-facing fields are a subset of the full server-side state.
struct GameStateSnapshot {
    std::array<int, 4>                    hand;
    std::array<std::optional<int32_t>, 4> best_bid;
    std::array<std::optional<int32_t>, 4> best_ask;
    std::array<std::optional<int32_t>, 4> last_trade_price;
    std::vector<std::array<int, 4>>       delta_table;
    int     player_slot      = -1;
    int     player_count     = 0;
    float   time_remaining_s = 0.0f;
    float   round_duration_s = 0.0f;
    int32_t balance          = 0;
    int32_t buy_in           = 0;
    int32_t points_per_card  = 0;
    bool    round_active     = false;
};

} // namespace anjeer::engine
