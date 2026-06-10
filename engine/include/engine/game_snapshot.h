#pragma once

#include "engine/suit.h"
#include "engine/order_book.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

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

// Shared read-only view of game state.
//
// Bot-facing fields (hand, balance, best_bid, best_ask, last_trade_price,
// round_active, round_duration_s, buy_in, points_per_card) are a subset
// populated by BotAdapter from per-bot wire events.
//
// Eval-facing fields (hands, balances, player_names, slot_active, deltas,
// books, recent_trades, round_number, deck_table, current_deck_index) are
// populated by GameSession::make_eval_snapshot() at round boundaries and
// after each trade; bots never read these.
struct GameStateSnapshot {
    // ── Bot-facing ────────────────────────────────────────────────────────
    std::array<int, 4>                    hand{};
    std::array<std::optional<int32_t>, 4> best_bid;
    std::array<std::optional<int32_t>, 4> best_ask;
    std::array<std::optional<int32_t>, 4> best_bid_qty;
    std::array<std::optional<int32_t>, 4> best_ask_qty;
    std::array<std::optional<int32_t>, 4> last_trade_price;
    int     my_slot          = -1;
    int     num_active_slots = 0;
    double  time_remaining_s = 0.0;
    float   round_duration_s = 0.0f;
    int32_t balance          = 0;
    int32_t buy_in           = 0;
    int32_t points_per_card  = 0;
    bool    round_active     = false;

    // ── Eval-facing (server-side only; never sent over wire) ──────────────
    std::array<std::array<int, 4>, 4> hands{};        // [slot][suit]
    std::array<int32_t, 4>           balances{};
    std::array<std::string, 4>       player_names{};
    std::array<bool, 4>              slot_active{};
    std::array<std::array<int, 4>, 4> deltas{};        // net card flow [slot][suit]
    std::array<BookSnapshot, 4>      books{};
    int                              round_number        = 0;
    // deck_table is populated by tests that configure BayesianEvalModule directly.
    // make_eval_snapshot() does not populate it; production uses EvalRunner::init_session().
    std::array<DeckSpec, 12>         deck_table{};
    int                              current_deck_index  = -1;
};

} // namespace anjeer::engine
