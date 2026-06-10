#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <variant>
#include <vector>

#include "exchange/order_book.h"

// AGENT-CTX: Moved from engine/tests/test_order_book.cpp as part of Slice 13
// (Task 3 — OrderBook migration). These are the authoritative Slice 2 acceptance
// criteria tests for OrderBook, now living in the exchange module where OrderBook
// resides. Namespace changed from anjeer::engine to anjeer::exchange; all type
// names and test logic are identical. Do not remove a test without removing the
// corresponding AC from the slice definitions.

using namespace anjeer::exchange;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// AGENT-CTX: make_config centralises default values so tests are insulated from
// config changes. If a test needs non-default bounds, pass them explicitly.
static OrderBook::Config make_config(int32_t min_price = 1, int32_t max_price = 99) {
    OrderBook::Config cfg;
    cfg.min_price               = min_price;
    cfg.max_price               = max_price;
    cfg.nudge_initial_buy_price  = 1;
    cfg.nudge_initial_sell_price = 99;
    cfg.suit                    = "S1";
    return cfg;
}

// Returns a pointer to the first event of type T in the vector, or nullptr.
// AGENT-CTX: Using std::get_if (not std::get) avoids throwing on wrong type.
// Tests use REQUIRE(find_event<T>(...) != nullptr) as a guard before dereferencing.
template<typename T>
static const T* find_event(const std::vector<OrderEvent>& events) {
    for (const auto& ev : events) {
        if (const T* p = std::get_if<T>(&ev)) return p;
    }
    return nullptr;
}

template<typename T>
static bool has_event(const std::vector<OrderEvent>& events) {
    return find_event<T>(events) != nullptr;
}

// ---------------------------------------------------------------------------
// AC: Player can submit a limit order and it appears in the order book
// ---------------------------------------------------------------------------

TEST_CASE("submit buy order returns OrderAckEvent with correct fields", "[order_book]") {
    OrderBook book{make_config()};
    auto events = book.submit(/*player_id=*/1, Side::Buy, /*price=*/50);

    REQUIRE(has_event<OrderAckEvent>(events));
    const auto* ack = find_event<OrderAckEvent>(events);
    CHECK(ack->player_id == 1);
    CHECK(ack->side      == Side::Buy);
    CHECK(ack->price     == 50);
    CHECK(ack->order_id  > 0);
    CHECK(ack->suit      == "S1");
}

TEST_CASE("submit updates best_bid after a buy order", "[order_book]") {
    OrderBook book{make_config()};
    book.submit(1, Side::Buy, 50);
    CHECK(book.best_bid() == 50);
    CHECK(book.best_ask() == std::nullopt);
}

TEST_CASE("submit returns BookUpdateEvent reflecting current best bid/ask", "[order_book]") {
    OrderBook book{make_config()};
    auto events = book.submit(1, Side::Buy, 50);

    REQUIRE(has_event<BookUpdateEvent>(events));
    const auto* upd = find_event<BookUpdateEvent>(events);
    CHECK(upd->suit     == "S1");
    CHECK(upd->best_bid == 50);
    CHECK(upd->best_ask == std::nullopt);
}

// ---------------------------------------------------------------------------
// AC: Order sits in the book until matched
// ---------------------------------------------------------------------------

TEST_CASE("unmatched buy and sell at different prices produce no TradeEvent", "[order_book]") {
    OrderBook book{make_config()};
    book.submit(1, Side::Buy,  40);
    auto events = book.submit(2, Side::Sell, 60);

    CHECK_FALSE(has_event<TradeEvent>(events));
    CHECK(book.best_bid() == 40);
    CHECK(book.best_ask() == 60);
}

TEST_CASE("empty book has nullopt best_bid and best_ask", "[order_book]") {
    OrderBook book{make_config()};
    CHECK(book.best_bid() == std::nullopt);
    CHECK(book.best_ask() == std::nullopt);
}

TEST_CASE("submit to empty book produces no TradeEvent", "[order_book]") {
    OrderBook book{make_config()};
    auto events = book.submit(1, Side::Buy, 50);
    CHECK_FALSE(has_event<TradeEvent>(events));
}

// ---------------------------------------------------------------------------
// AC: Matching order (same price, opposite side) causes immediate execution
// ---------------------------------------------------------------------------

TEST_CASE("buy and sell at same price produce TradeEvent", "[order_book]") {
    OrderBook book{make_config()};
    book.submit(1, Side::Buy,  50);
    auto events = book.submit(2, Side::Sell, 50);

    REQUIRE(has_event<TradeEvent>(events));
    const auto* trade = find_event<TradeEvent>(events);
    CHECK(trade->suit      == "S1");
    CHECK(trade->price     == 50);
    CHECK(trade->buyer_id  == 1);
    CHECK(trade->seller_id == 2);
}

TEST_CASE("buy above resting ask price also triggers trade", "[order_book]") {
    OrderBook book{make_config()};
    book.submit(1, Side::Sell, 50);  // resting ask at 50
    auto events = book.submit(2, Side::Buy, 55);  // aggressive buy above ask

    REQUIRE(has_event<TradeEvent>(events));
    const auto* trade = find_event<TradeEvent>(events);
    CHECK(trade->buyer_id  == 2);
    CHECK(trade->seller_id == 1);
}

// AGENT-CTX: Execution price is the MAKER's (resting order's) price — the standard
// financial convention. The aggressor always gets a price at least as good as submitted.
// Example: resting bid at 55, aggressive sell at 50 → executes at 55.
// If this convention is changed, update the wire protocol and frontend display too.
TEST_CASE("trade executes at the resting (maker) order price", "[order_book]") {
    OrderBook book{make_config()};
    book.submit(1, Side::Buy, 55);        // resting bid at 55
    auto events = book.submit(2, Side::Sell, 50);  // aggressive sell at 50

    REQUIRE(has_event<TradeEvent>(events));
    const auto* trade = find_event<TradeEvent>(events);
    CHECK(trade->price == 55);            // maker's price, not taker's 50
}

TEST_CASE("aggressor_side is the side that crossed the book", "[order_book]") {
    OrderBook book{make_config()};
    book.submit(1, Side::Buy,  50);
    auto events = book.submit(2, Side::Sell, 50);  // sell is the aggressor

    const auto* trade = find_event<TradeEvent>(events);
    REQUIRE(trade != nullptr);
    CHECK(trade->aggressor_side == Side::Sell);
}

TEST_CASE("buy aggressor sets aggressor_side to Buy", "[order_book]") {
    OrderBook book{make_config()};
    book.submit(1, Side::Sell, 50);      // resting ask
    auto events = book.submit(2, Side::Buy, 50);  // buy is the aggressor

    const auto* trade = find_event<TradeEvent>(events);
    REQUIRE(trade != nullptr);
    CHECK(trade->aggressor_side == Side::Buy);
}

// ---------------------------------------------------------------------------
// AC: Price-time priority — earlier order at same price matches first
// ---------------------------------------------------------------------------

TEST_CASE("price-time priority: earlier bid at equal price is matched first", "[order_book]") {
    OrderBook book{make_config()};
    book.submit(/*player_id=*/1, Side::Buy, 50);  // earlier (lower id)
    book.submit(/*player_id=*/2, Side::Buy, 50);  // later  (higher id)

    auto events = book.submit(/*player_id=*/3, Side::Sell, 50);

    const auto* trade = find_event<TradeEvent>(events);
    REQUIRE(trade != nullptr);
    CHECK(trade->buyer_id  == 1);  // player 1 was first in queue at price 50
    CHECK(trade->seller_id == 3);
}

TEST_CASE("higher price bid takes priority over earlier lower price bid", "[order_book]") {
    OrderBook book{make_config()};
    book.submit(1, Side::Buy, 40);   // earlier, lower price
    book.submit(2, Side::Buy, 50);   // later, higher price → higher priority

    auto events = book.submit(3, Side::Sell, 40);

    const auto* trade = find_event<TradeEvent>(events);
    REQUIRE(trade != nullptr);
    CHECK(trade->buyer_id == 2);     // bid at 50 matched, not bid at 40
}

// ---------------------------------------------------------------------------
// AC: Order book state (best bid, best ask) broadcast after every change
// ---------------------------------------------------------------------------

TEST_CASE("book_update is always the last event in a submit result", "[order_book]") {
    OrderBook book{make_config()};
    auto events = book.submit(1, Side::Buy, 50);

    REQUIRE_FALSE(events.empty());
    // AGENT-CTX: BookUpdateEvent must be the last event so the server broadcasts
    // the final book state after all other events (including TradeEvent) are processed.
    CHECK(std::holds_alternative<BookUpdateEvent>(events.back()));
}

TEST_CASE("book_update after trade shows empty book (wipe contract applied)", "[order_book]") {
    // AGENT-CTX: After a trade, the server calls wipe() on ALL books (wipe contract).
    // wipe() returns a BookUpdateEvent with nullopt best_bid/best_ask that the server
    // broadcasts — it never constructs post-wipe book updates itself.
    OrderBook book{make_config()};
    book.submit(1, Side::Buy,  50);
    book.submit(2, Side::Sell, 50);  // triggers trade
    const auto events = book.wipe();

    CHECK(book.best_bid() == std::nullopt);
    CHECK(book.best_ask() == std::nullopt);
    REQUIRE(has_event<BookUpdateEvent>(events));
    const auto* upd = find_event<BookUpdateEvent>(events);
    CHECK(upd->best_bid == std::nullopt);
    CHECK(upd->best_ask == std::nullopt);
}


// ---------------------------------------------------------------------------
// AC: Malformed orders are rejected with a typed error message — no crashes
// ---------------------------------------------------------------------------

TEST_CASE("submit with price below min_price returns OrderErrorEvent", "[order_book]") {
    OrderBook book{make_config()};
    auto events = book.submit(1, Side::Buy, /*price=*/0);

    REQUIRE(has_event<OrderErrorEvent>(events));
    const auto* err = find_event<OrderErrorEvent>(events);
    CHECK(err->code == OrderErrorEvent::Code::PriceOutOfRange);
    CHECK_FALSE(err->message.empty());
}

TEST_CASE("submit with price above max_price returns OrderErrorEvent", "[order_book]") {
    OrderBook book{make_config()};
    auto events = book.submit(1, Side::Buy, /*price=*/100);

    REQUIRE(has_event<OrderErrorEvent>(events));
    const auto* err = find_event<OrderErrorEvent>(events);
    CHECK(err->code == OrderErrorEvent::Code::PriceOutOfRange);
}

// ---------------------------------------------------------------------------
// AC: Self-trade suppression
// ---------------------------------------------------------------------------

TEST_CASE("self-trade buy into own ask: ask erased to unblock book", "[order_book]") {
    OrderBook book{make_config()};
    book.submit(1, Side::Sell, 50);          // player 1 resting ask at 50

    auto events = book.submit(1, Side::Buy, 50);  // player 1 buys at 50 — crosses own ask

    CHECK_FALSE(has_event<TradeEvent>(events));  // no trade
    REQUIRE(has_event<OrderAckEvent>(events));   // bid acked
    CHECK(book.best_bid() == 50);               // bid remains
    CHECK_FALSE(book.best_ask().has_value());   // ask erased to unblock
}

TEST_CASE("self-trade sell into own bid: ask erased to unblock book", "[order_book]") {
    OrderBook book{make_config()};
    book.submit(1, Side::Buy, 50);           // player 1 resting bid at 50

    auto events = book.submit(1, Side::Sell, 50); // player 1 sells at 50 — crosses own bid

    CHECK_FALSE(has_event<TradeEvent>(events));
    REQUIRE(has_event<OrderAckEvent>(events));
    CHECK(book.best_bid() == 50);               // bid remains
    CHECK_FALSE(book.best_ask().has_value());   // ask erased to unblock
}

TEST_CASE("rejected order does not modify the book", "[order_book]") {
    OrderBook book{make_config()};
    book.submit(1, Side::Buy, 50);  // valid order first
    book.submit(1, Side::Buy,  0);  // invalid — should not change book

    CHECK(book.best_bid() == 50);   // book unchanged
}

// ---------------------------------------------------------------------------
// AC: Cancel
// ---------------------------------------------------------------------------

TEST_CASE("cancel valid order returns OrderCancelAckEvent", "[order_book]") {
    OrderBook book{make_config()};
    auto submit_events = book.submit(1, Side::Buy, 50);
    const auto* ack = find_event<OrderAckEvent>(submit_events);
    REQUIRE(ack != nullptr);

    auto cancel_events = book.cancel(ack->order_id, /*player_id=*/1);

    REQUIRE(has_event<OrderCancelAckEvent>(cancel_events));
    const auto* cack = find_event<OrderCancelAckEvent>(cancel_events);
    CHECK(cack->order_id == ack->order_id);
}

TEST_CASE("cancel removes order from book", "[order_book]") {
    OrderBook book{make_config()};
    auto submit_events = book.submit(1, Side::Buy, 50);
    const auto* ack = find_event<OrderAckEvent>(submit_events);
    REQUIRE(ack != nullptr);

    book.cancel(ack->order_id, /*player_id=*/1);

    CHECK(book.best_bid() == std::nullopt);
}

TEST_CASE("cancel with unknown order_id returns OrderNotFound error", "[order_book]") {
    OrderBook book{make_config()};
    auto events = book.cancel(/*order_id=*/9999, /*player_id=*/1);

    REQUIRE(has_event<OrderErrorEvent>(events));
    const auto* err = find_event<OrderErrorEvent>(events);
    CHECK(err->code == OrderErrorEvent::Code::OrderNotFound);
}

TEST_CASE("cancel order belonging to another player returns NotYourOrder error", "[order_book]") {
    OrderBook book{make_config()};
    auto submit_events = book.submit(/*player_id=*/1, Side::Buy, 50);
    const auto* ack = find_event<OrderAckEvent>(submit_events);
    REQUIRE(ack != nullptr);

    // Player 2 tries to cancel player 1's order
    auto cancel_events = book.cancel(ack->order_id, /*player_id=*/2);

    REQUIRE(has_event<OrderErrorEvent>(cancel_events));
    const auto* err = find_event<OrderErrorEvent>(cancel_events);
    CHECK(err->code == OrderErrorEvent::Code::NotYourOrder);
    // Order must still be in the book after a failed cancel
    CHECK(book.best_bid() == 50);
}

// ---------------------------------------------------------------------------
// AC: wipe() — global wipe contract
// ---------------------------------------------------------------------------

TEST_CASE("wipe() removes all resting bids and asks", "[order_book]") {
    OrderBook book{make_config()};
    book.submit(1, Side::Buy,  40);
    book.submit(2, Side::Sell, 60);

    book.wipe();

    CHECK(book.best_bid() == std::nullopt);
    CHECK(book.best_ask() == std::nullopt);
}

TEST_CASE("wipe() returns BookUpdateEvent with null best_bid and best_ask", "[order_book]") {
    OrderBook book{make_config()};
    book.submit(1, Side::Buy,  40);
    book.submit(2, Side::Sell, 60);

    const auto events = book.wipe();

    REQUIRE(has_event<BookUpdateEvent>(events));
    const auto* upd = find_event<BookUpdateEvent>(events);
    CHECK(upd->suit     == "S1");
    CHECK(upd->best_bid == std::nullopt);
    CHECK(upd->best_ask == std::nullopt);
}

TEST_CASE("cancel after wipe returns OrderNotFound (orders no longer exist)", "[order_book]") {
    OrderBook book{make_config()};
    auto submit_events = book.submit(1, Side::Buy, 50);
    const auto* ack = find_event<OrderAckEvent>(submit_events);
    REQUIRE(ack != nullptr);
    const int64_t saved_id = ack->order_id;

    book.wipe();

    auto cancel_events = book.cancel(saved_id, /*player_id=*/1);
    REQUIRE(has_event<OrderErrorEvent>(cancel_events));
    CHECK(find_event<OrderErrorEvent>(cancel_events)->code == OrderErrorEvent::Code::OrderNotFound);
}

// ---------------------------------------------------------------------------
// Partial fill tests (Slice 15 Task 3)
// ---------------------------------------------------------------------------

TEST_CASE("submit qty=1 exact fill — backward compat", "[order_book][partial_fill]") {
    OrderBook book{make_config()};
    book.submit(/*player_id=*/2, Side::Sell, 50, 1);
    auto events = book.submit(/*player_id=*/1, Side::Buy, 50, 1);

    const auto* trade = find_event<TradeEvent>(events);
    REQUIRE(trade != nullptr);
    CHECK(trade->qty_filled == 1);
    CHECK(book.bids_snapshot().empty());
    CHECK(book.asks_snapshot().empty());
}

TEST_CASE("submit qty=3 partial against resting qty=1 — aggressor larger", "[order_book][partial_fill]") {
    OrderBook book{make_config()};
    book.submit(/*player_id=*/2, Side::Sell, 50, 1);
    auto events = book.submit(/*player_id=*/1, Side::Buy, 50, 3);

    const auto* trade = find_event<TradeEvent>(events);
    REQUIRE(trade != nullptr);
    CHECK(trade->qty_filled == 1);

    // Aggressor (buy) has 2 remaining, resting sell fully consumed.
    CHECK(book.asks_snapshot().empty());
    REQUIRE(book.bids_snapshot().size() == 1);
    CHECK(book.bids_snapshot().front().price == 50);
}

TEST_CASE("submit qty=5 consumes two resting orders", "[order_book][partial_fill]") {
    OrderBook book{make_config()};
    book.submit(/*player_id=*/2, Side::Sell, 50, 2);
    book.submit(/*player_id=*/3, Side::Sell, 50, 2);
    auto events = book.submit(/*player_id=*/1, Side::Buy, 50, 5);

    int trade_count = 0;
    int total_filled = 0;
    for (const auto& ev : events) {
        if (const auto* t = std::get_if<TradeEvent>(&ev)) {
            ++trade_count;
            total_filled += t->qty_filled;
        }
    }
    CHECK(trade_count == 2);
    CHECK(total_filled == 4);

    // Aggressor (buy) has 1 remaining; both resting sells consumed.
    CHECK(book.asks_snapshot().empty());
    REQUIRE(book.bids_snapshot().size() == 1);
}

TEST_CASE("resting order partially filled by smaller aggressor", "[order_book][partial_fill]") {
    OrderBook book{make_config()};
    book.submit(/*player_id=*/2, Side::Sell, 50, 5);
    auto events = book.submit(/*player_id=*/1, Side::Buy, 50, 2);

    const auto* trade = find_event<TradeEvent>(events);
    REQUIRE(trade != nullptr);
    CHECK(trade->qty_filled == 2);

    // Aggressor (buy) fully consumed; resting sell has 3 remaining.
    CHECK(book.bids_snapshot().empty());
    REQUIRE(book.asks_snapshot().size() == 1);
    CHECK(book.asks_snapshot().front().price == 50);
}

TEST_CASE("submit qty=3 no match inserts full qty", "[order_book][partial_fill]") {
    OrderBook book{make_config()};
    auto events = book.submit(/*player_id=*/1, Side::Buy, 50, 3);

    const auto* ack = find_event<OrderAckEvent>(events);
    REQUIRE(ack != nullptr);
    CHECK(ack->qty == 3);
    REQUIRE(!find_event<TradeEvent>(events));

    REQUIRE(book.bids_snapshot().size() == 1);
    CHECK(book.bids_snapshot().front().price == 50);
}
