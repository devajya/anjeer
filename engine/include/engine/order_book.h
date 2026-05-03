#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace anjeer::engine {

// ---------------------------------------------------------------------------
// Side
// ---------------------------------------------------------------------------
enum class Side { Buy, Sell };

// ---------------------------------------------------------------------------
// Event types returned by OrderBook operations.
//
// std::variant is used over virtual dispatch for the small fixed set of event
// types; the server uses std::visit to handle each variant.
// Events carry engine-domain data only — never JSON, string formatting, or WS handles.
// ---------------------------------------------------------------------------

// Confirms a successfully placed order. Sent only to the submitting client.
struct OrderAckEvent {
    int64_t     order_id;
    int32_t     player_id;
    Side        side;
    int32_t     price;
    std::string suit;
};

// Emitted when a buy and sell cross. Sent to all clients; the server personalises
// your_side per recipient before serialising.
// qty is absent — every trade is implicitly qty=1 in the current game mechanic.
struct TradeEvent {
    std::string suit;
    int32_t     price;
    int32_t     buyer_id;
    int32_t     seller_id;
    Side        aggressor_side;
};

// Broadcast to all clients after every mutation that changes best bid or ask.
// best_bid / best_ask are nullopt when no orders exist on that side.
// best_bid_player_id / best_ask_player_id are nullopt when no orders exist.
// In this codebase player_id passed to the engine equals the player's slot index.
struct BookUpdateEvent {
    std::string            suit;
    std::optional<int32_t> best_bid;
    std::optional<int32_t> best_ask;
    std::optional<int32_t> best_bid_player_id;
    std::optional<int32_t> best_ask_player_id;
};

// Confirms a successfully cancelled order. Sent only to the cancelling client.
struct OrderCancelAckEvent {
    int64_t order_id;
};

// Returned when an operation fails validation. Sent only to the relevant client.
// Error codes are a closed enum — add values here when new validation rules are introduced.
// Server-layer rejections (unknown suit, malformed JSON) are handled before the engine
// is called and do not belong in this enum.
struct OrderErrorEvent {
    enum class Code {
        PriceOutOfRange,
        OrderNotFound,
        NotYourOrder,
        SelfTrade,
    };
    Code        code;
    std::string message;  // human-readable; may change — do not parse on the client
};

// Complete set of results an OrderBook operation can produce.
// A single call may return multiple events (e.g. OrderAck + TradeEvent + BookUpdateEvent).
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
//   - bids_ is sorted descending by price, ascending by id on ties (FIFO within price).
//   - asks_ is sorted ascending by price, ascending by id on ties.
//   - No order in bids_ has price >= any order in asks_ (no resting crossed book).
//   - Every order has a unique id > 0.
//   - All prices satisfy min_price <= price <= max_price.
//
// WIPE CONTRACT (global mechanic, Slice 2–?):
//   When any submit() or nudge() result contains a TradeEvent, the caller must
//   call wipe() on ALL active OrderBook instances (one per suit). The engine does
//   NOT self-wipe; the global wipe is a game-mechanic policy triggered by the server.
//   wipe() clears all resting orders and returns a BookUpdateEvent{suit, nullopt, nullopt}.
// ---------------------------------------------------------------------------
class OrderBook {
public:
    struct Config {
        int32_t     min_price               = 1;
        int32_t     max_price               = 99;
        int32_t     nudge_initial_buy_price  = 1;   // fallback when no bids exist
        int32_t     nudge_initial_sell_price = 99;  // fallback when no asks exist
        std::string suit                    = "S1";
    };

    explicit OrderBook(Config cfg);

    // Submit a new limit order.
    // Returns: [OrderAckEvent] + optional [TradeEvent, BookUpdateEvent]
    //       or: [OrderErrorEvent] on validation failure.
    // If the result contains a TradeEvent, the caller MUST invoke wipe() on ALL
    // OrderBook instances (see WIPE CONTRACT above).
    [[nodiscard]] std::vector<OrderEvent> submit(int32_t player_id, Side side, int32_t price);

    // Cancel a resting order by ID.
    // Returns: [OrderCancelAckEvent, BookUpdateEvent]
    //       or: [OrderErrorEvent] (OrderNotFound or NotYourOrder).
    // Precondition: pass the authenticated player_id — the engine enforces ownership.
    [[nodiscard]] std::vector<OrderEvent> cancel(int64_t order_id, int32_t player_id);

    // Place a new order at best_bid+1 (buy) or best_ask-1 (sell).
    // Falls back to nudge_initial_buy_price (if no bids) or nudge_initial_sell_price
    // (if no asks). Price is clamped to [min_price, max_price].
    // Always creates a NEW order — does not modify an existing order.
    // Returns same shape as submit().
    [[nodiscard]] std::vector<OrderEvent> nudge(Side side, int32_t player_id);

    // Remove all resting orders from this book and return a BookUpdateEvent
    // with null best_bid / best_ask (the post-wipe state).
    // Called by the server on ALL active books after any TradeEvent (global wipe).
    [[nodiscard]] std::vector<OrderEvent> wipe();

    [[nodiscard]] std::optional<int32_t> best_bid() const noexcept;
    [[nodiscard]] std::optional<int32_t> best_ask() const noexcept;

private:
    struct Order {
        int64_t id;
        int32_t player_id;
        Side    side;
        int32_t price;
    };

    Config  cfg_;
    int64_t next_id_{1};  // ID 0 is reserved as a sentinel; first issued ID is 1

    // Sort invariant: bids_ descending price, ascending id on ties.
    //                 asks_ ascending price, ascending id on ties.
    std::vector<Order> bids_;
    std::vector<Order> asks_;

    // Check whether best_bid >= best_ask and return a TradeEvent if so.
    // Removes the matched orders from bids_ and asks_.
    std::optional<TradeEvent> try_match();

    // Construct a BookUpdateEvent snapshot for the current book state.
    [[nodiscard]] BookUpdateEvent make_book_update() const;

    [[nodiscard]] bool is_valid_price(int32_t price) const noexcept;
};

} // namespace anjeer::engine
