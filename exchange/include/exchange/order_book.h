#pragma once

#include "exchange/exchange_types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace anjeer::exchange {

struct OrderAckEvent {
    int64_t     order_id;
    int32_t     player_id;
    Side        side;
    int32_t     price;
    std::string suit;
    int32_t     qty;
};

// Emitted when a buy and sell cross; server personalises your_side per recipient.
struct TradeEvent {
    std::string suit;
    int32_t     price;
    int32_t     buyer_id;
    int32_t     seller_id;
    Side        aggressor_side;
    int32_t     qty_filled;
};

// Broadcast after every mutation that changes best bid or ask.
struct BookUpdateEvent {
    std::string            suit;
    std::optional<int32_t> best_bid;
    std::optional<int32_t> best_ask;
    std::optional<int32_t> best_bid_player_id;
    std::optional<int32_t> best_ask_player_id;
};

struct OrderCancelAckEvent {
    int64_t order_id;
};

struct OrderErrorEvent {
    enum class Code {
        PriceOutOfRange,
        OrderNotFound,
        NotYourOrder,
        SelfTrade,
    };
    Code        code;
    std::string message;
};

using OrderEvent = std::variant<
    OrderAckEvent,
    TradeEvent,
    BookUpdateEvent,
    OrderCancelAckEvent,
    OrderErrorEvent
>;

// ---------------------------------------------------------------------------
// OrderBook — single-suit, price-time priority limit order book.
//
// Invariants (must hold after every public method call):
//   - bids_ sorted descending by price, ascending by id on ties (FIFO within price).
//   - asks_ sorted ascending by price, ascending by id on ties.
//   - No order in bids_ has price >= any order in asks_ (no resting crossed book).
//   - Every order has a unique id > 0.
//   - All prices satisfy min_price <= price <= max_price.
//
// WIPE CONTRACT: when any submit()/nudge() result contains a TradeEvent, the
// caller must call wipe() on ALL active OrderBook instances. The engine does
// NOT self-wipe; wipe is a game-mechanic policy owned by the server.
// ---------------------------------------------------------------------------
class OrderBook {
public:
    struct Config {
        int32_t     min_price               = 1;
        int32_t     max_price               = 99;
        int32_t     nudge_initial_buy_price  = 1;
        int32_t     nudge_initial_sell_price = 99;
        std::string suit                    = "S1";
    };

    explicit OrderBook(Config cfg);

    // Returns: [OrderAckEvent] + optional [TradeEvent, BookUpdateEvent]
    //       or: [OrderErrorEvent] on validation failure.
    [[nodiscard]] std::vector<OrderEvent> submit(int32_t player_id, Side side, int32_t price, int32_t qty = 1);

    // Returns: [OrderCancelAckEvent, BookUpdateEvent]
    //       or: [OrderErrorEvent] (OrderNotFound or NotYourOrder).
    [[nodiscard]] std::vector<OrderEvent> cancel(int64_t order_id, int32_t player_id);

    // Clear all resting orders; returns BookUpdateEvent with null best_bid/ask.
    [[nodiscard]] std::vector<OrderEvent> wipe();

    [[nodiscard]] std::optional<int32_t> best_bid() const noexcept;
    [[nodiscard]] std::optional<int32_t> best_ask() const noexcept;

    struct OrderSnapshot { int64_t order_id; int32_t price; int32_t player_slot; };

    [[nodiscard]] std::vector<OrderEvent>      cancel_player(int32_t player_id);
    [[nodiscard]] std::vector<OrderSnapshot>   bids_snapshot() const;
    [[nodiscard]] std::vector<OrderSnapshot>   asks_snapshot() const;

private:
    struct Order {
        int64_t id;
        int32_t player_id;
        Side    side;
        int32_t price;
        int32_t qty;
    };

    Config  cfg_;
    int64_t next_id_{1};

    std::vector<Order> bids_;
    std::vector<Order> asks_;

    std::vector<TradeEvent> match_loop();
    [[nodiscard]] BookUpdateEvent make_book_update() const;
    [[nodiscard]] bool is_valid_price(int32_t price) const noexcept;
};

} // namespace anjeer::exchange
