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
// AC: Nudge mechanic
// ---------------------------------------------------------------------------

TEST_CASE("nudge buy with empty book creates order at nudge_initial_buy_price", "[order_book]") {
    OrderBook book{make_config()};
    auto events = book.nudge(Side::Buy, /*player_id=*/1);

    REQUIRE(has_event<OrderAckEvent>(events));
    const auto* ack = find_event<OrderAckEvent>(events);
    CHECK(ack->price == 1);  // nudge_initial_buy_price from config
    CHECK(book.best_bid() == 1);
}

TEST_CASE("nudge buy with existing bids creates order at best_bid + 1", "[order_book]") {
    OrderBook book{make_config()};
    book.submit(1, Side::Buy, 45);

    auto events = book.nudge(Side::Buy, /*player_id=*/2);

    REQUIRE(has_event<OrderAckEvent>(events));
    const auto* ack = find_event<OrderAckEvent>(events);
    CHECK(ack->price == 46);         // best_bid(45) + 1
    CHECK(book.best_bid() == 46);
}

TEST_CASE("nudge sell with empty book creates order at nudge_initial_sell_price", "[order_book]") {
    OrderBook book{make_config()};
    auto events = book.nudge(Side::Sell, /*player_id=*/1);

    REQUIRE(has_event<OrderAckEvent>(events));
    const auto* ack = find_event<OrderAckEvent>(events);
    CHECK(ack->price == 99);  // nudge_initial_sell_price from config
    CHECK(book.best_ask() == 99);
}

TEST_CASE("nudge sell with existing asks creates order at best_ask - 1", "[order_book]") {
    OrderBook book{make_config()};
    book.submit(1, Side::Sell, 60);

    auto events = book.nudge(Side::Sell, /*player_id=*/2);

    REQUIRE(has_event<OrderAckEvent>(events));
    const auto* ack = find_event<OrderAckEvent>(events);
    CHECK(ack->price == 59);         // best_ask(60) - 1
    CHECK(book.best_ask() == 59);
}

TEST_CASE("nudge buy that crosses best ask produces TradeEvent", "[order_book]") {
    OrderBook book{make_config()};
    book.submit(1, Side::Sell, 51);  // resting ask at 51
    book.submit(2, Side::Buy,  50);  // resting bid at 50

    // Nudge buy: price = best_bid(50) + 1 = 51, which crosses ask at 51
    auto events = book.nudge(Side::Buy, /*player_id=*/3);

    REQUIRE(has_event<TradeEvent>(events));
    const auto* trade = find_event<TradeEvent>(events);
    CHECK(trade->buyer_id  == 3);
    CHECK(trade->seller_id == 1);
}

TEST_CASE("nudge sell that crosses best bid produces TradeEvent", "[order_book]") {
    OrderBook book{make_config()};
    book.submit(1, Side::Buy,  49);  // resting bid at 49
    book.submit(2, Side::Sell, 50);  // resting ask at 50

    // Nudge sell: price = best_ask(50) - 1 = 49, which crosses bid at 49
    auto events = book.nudge(Side::Sell, /*player_id=*/3);

    REQUIRE(has_event<TradeEvent>(events));
    const auto* trade = find_event<TradeEvent>(events);
    CHECK(trade->buyer_id  == 1);
    CHECK(trade->seller_id == 3);
}

TEST_CASE("nudge buy at max_price clamps to max_price and does not error", "[order_book]") {
    OrderBook book{make_config(/*min=*/1, /*max=*/99)};
    book.submit(1, Side::Buy, 99);  // resting bid already at ceiling

    // Nudge buy: would be 100, clamped to 99
    auto events = book.nudge(Side::Buy, /*player_id=*/2);

    CHECK_FALSE(has_event<OrderErrorEvent>(events));
    REQUIRE(has_event<OrderAckEvent>(events));
    const auto* ack = find_event<OrderAckEvent>(events);
    CHECK(ack->price == 99);  // clamped
}

TEST_CASE("nudge sell at min_price clamps to min_price and does not error", "[order_book]") {
    OrderBook book{make_config(/*min=*/1, /*max=*/99)};
    book.submit(1, Side::Sell, 1);   // resting ask already at floor

    // Nudge sell: would be 0, clamped to 1
    auto events = book.nudge(Side::Sell, /*player_id=*/2);

    CHECK_FALSE(has_event<OrderErrorEvent>(events));
    REQUIRE(has_event<OrderAckEvent>(events));
    const auto* ack = find_event<OrderAckEvent>(events);
    CHECK(ack->price == 1);   // clamped
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
