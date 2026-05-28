#include <catch2/catch_test_macros.hpp>
#include "exchange/exchange_session.h"

using namespace anjeer::exchange;

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

static ExchangeSession make_session() {
    return ExchangeSession({{1, 99, 1, 99, "C"},
                             {1, 99, 1, 99, "D"},
                             {1, 99, 1, 99, "H"},
                             {1, 99, 1, 99, "S"}});
}

template<class T>
static bool has_feedback(const ExchangeResult& r) {
    for (auto& f : r.feedback)
        if (std::holds_alternative<T>(f)) return true;
    return false;
}

template<class T>
static bool has_market(const ExchangeResult& r) {
    for (auto& m : r.market)
        if (std::holds_alternative<T>(m)) return true;
    return false;
}

template<class T>
static const T& get_feedback(const ExchangeResult& r) {
    for (auto& f : r.feedback)
        if (std::holds_alternative<T>(f)) return std::get<T>(f);
    throw std::logic_error("feedback type not found");
}

template<class T>
static const T& get_market(const ExchangeResult& r) {
    for (auto& m : r.market)
        if (std::holds_alternative<T>(m)) return std::get<T>(m);
    throw std::logic_error("market type not found");
}

template<class T>
static std::vector<T> filter_feedback(const ExchangeResult& r) {
    std::vector<T> out;
    for (auto& f : r.feedback)
        if (std::holds_alternative<T>(f)) out.push_back(std::get<T>(f));
    return out;
}

template<class T>
static std::vector<T> filter_market(const ExchangeResult& r) {
    std::vector<T> out;
    for (auto& m : r.market)
        if (std::holds_alternative<T>(m)) out.push_back(std::get<T>(m));
    return out;
}

// ---------------------------------------------------------------------------
// submit_order
// ---------------------------------------------------------------------------

TEST_CASE("submit_order non-crossing: OrderAck in feedback, OrderAdded in market") {
    auto s = make_session();
    auto r = s.submit_order(0, Side::Buy, 50, 1);
    REQUIRE(has_feedback<OrderAck>(r));
    REQUIRE(has_market<OrderAdded>(r));

    auto& ack   = get_feedback<OrderAck>(r);
    auto& added = get_market<OrderAdded>(r);

    REQUIRE(ack.instrument_id == 0);
    REQUIRE(ack.side == Side::Buy);
    REQUIRE(ack.price == 50);
    REQUIRE(ack.player_slot == 1);

    REQUIRE(added.instrument_id == 0);
    REQUIRE(added.side == Side::Buy);
    REQUIRE(added.price == 50);
    REQUIRE(added.player_slot == 1);
    REQUIRE(added.seq == 1);
    REQUIRE(added.v == 1);
}

TEST_CASE("submit_order non-crossing: order_id consistent between OrderAck and OrderAdded") {
    auto s = make_session();
    auto r = s.submit_order(0, Side::Buy, 40, 2);
    REQUIRE(get_feedback<OrderAck>(r).order_id == get_market<OrderAdded>(r).order_id);
}

TEST_CASE("submit_order crossing: OrderExecuted in market with correct slots") {
    auto s = make_session();
    s.submit_order(0, Side::Sell, 50, 2);  // resting sell at 50 by slot 2
    auto r = s.submit_order(0, Side::Buy, 50, 1);  // crossing buy by slot 1

    REQUIRE(has_market<OrderExecuted>(r));
    auto& exec = get_market<OrderExecuted>(r);
    REQUIRE(exec.instrument_id == 0);
    REQUIRE(exec.price == 50);
    REQUIRE(exec.aggressor_side == Side::Buy);
    REQUIRE(exec.buyer_slot == 1);
    REQUIRE(exec.seller_slot == 2);
    REQUIRE(exec.v == 1);
}

TEST_CASE("submit_order crossing: BookUpdated in feedback reflects empty book post-trade") {
    auto s = make_session();
    s.submit_order(0, Side::Sell, 50, 2);
    auto r = s.submit_order(0, Side::Buy, 50, 1);

    REQUIRE(has_feedback<BookUpdated>(r));
    auto& upd = get_feedback<BookUpdated>(r);
    REQUIRE(upd.instrument_id == 0);
    REQUIRE_FALSE(upd.best_bid.has_value());
    REQUIRE_FALSE(upd.best_ask.has_value());
}

TEST_CASE("submit_order out of range: OrderRejected in feedback, market empty") {
    auto s = make_session();
    auto r = s.submit_order(0, Side::Buy, 200, 1);  // above max_price=99

    REQUIRE(has_feedback<OrderRejected>(r));
    REQUIRE(r.market.empty());
    REQUIRE(get_feedback<OrderRejected>(r).code == OrderRejected::Code::PriceOutOfRange);
}

// AGENT-CTX: SelfTrade is in OrderRejected::Code (mirrors OrderErrorEvent::Code)
// but OrderBook does not currently enforce self-trade prevention — the code
// exists for future use. No self-trade test is included here to avoid asserting
// behaviour the engine does not implement. Add a test when enforcement is added.

// ---------------------------------------------------------------------------
// cancel_order
// ---------------------------------------------------------------------------

TEST_CASE("cancel_order success: CancelAck in feedback, OrderCancelled in market") {
    auto s = make_session();
    auto sub = s.submit_order(0, Side::Buy, 40, 1);
    auto order_id = get_feedback<OrderAck>(sub).order_id;

    auto r = s.cancel_order(order_id, 0, 1);
    REQUIRE(has_feedback<CancelAck>(r));
    REQUIRE(has_market<OrderCancelled>(r));

    REQUIRE(get_feedback<CancelAck>(r).order_id == order_id);
    REQUIRE(get_feedback<CancelAck>(r).instrument_id == 0);
    REQUIRE(get_market<OrderCancelled>(r).order_id == order_id);
    REQUIRE(get_market<OrderCancelled>(r).instrument_id == 0);
    REQUIRE(get_market<OrderCancelled>(r).v == 1);
}

TEST_CASE("cancel_order not found: OrderRejected in feedback") {
    auto s = make_session();
    auto r = s.cancel_order(9999, 0, 1);

    REQUIRE(has_feedback<OrderRejected>(r));
    REQUIRE(get_feedback<OrderRejected>(r).code == OrderRejected::Code::OrderNotFound);
}

TEST_CASE("cancel_order wrong player: OrderRejected with NotYourOrder") {
    auto s = make_session();
    auto sub = s.submit_order(0, Side::Buy, 40, 1);
    auto order_id = get_feedback<OrderAck>(sub).order_id;

    auto r = s.cancel_order(order_id, 0, 2);  // slot 2 cancels slot 1's order
    REQUIRE(has_feedback<OrderRejected>(r));
    REQUIRE(get_feedback<OrderRejected>(r).code == OrderRejected::Code::NotYourOrder);
}

// ---------------------------------------------------------------------------
// cancel_player
// ---------------------------------------------------------------------------

TEST_CASE("cancel_player removes all orders for slot across all instruments") {
    auto s = make_session();
    s.submit_order(0, Side::Buy,  40, 3);
    s.submit_order(1, Side::Sell, 60, 3);
    s.submit_order(2, Side::Buy,  35, 3);

    auto r = s.cancel_player(3);
    auto acks = filter_feedback<CancelAck>(r);
    REQUIRE(acks.size() == 3);

    auto cancels = filter_market<OrderCancelled>(r);
    REQUIRE(cancels.size() == 3);
}

TEST_CASE("cancel_player on slot with no orders returns empty result") {
    auto s = make_session();
    auto r = s.cancel_player(99);
    REQUIRE(r.feedback.empty());
    REQUIRE(r.market.empty());
}

TEST_CASE("cancel_player does not cancel other players' orders") {
    auto s = make_session();
    s.submit_order(0, Side::Buy, 40, 1);
    s.submit_order(0, Side::Buy, 38, 2);

    auto r = s.cancel_player(1);
    auto acks = filter_feedback<CancelAck>(r);
    REQUIRE(acks.size() == 1);

    // slot 2's order still rests
    REQUIRE(s.best_bid(0).value() == 38);
}

// ---------------------------------------------------------------------------
// wipe
// ---------------------------------------------------------------------------

TEST_CASE("wipe clears all books and returns one BookUpdated per instrument") {
    auto s = make_session();
    s.submit_order(0, Side::Buy, 40, 1);
    s.submit_order(2, Side::Sell, 60, 2);

    auto updates = s.wipe();
    REQUIRE(updates.size() == 4);

    for (instrument_id_t i = 0; i < 4; ++i) {
        REQUIRE_FALSE(updates[i].best_bid.has_value());
        REQUIRE_FALSE(updates[i].best_ask.has_value());
    }
}

TEST_CASE("wipe on empty books still returns BookUpdated per instrument") {
    auto s = make_session();
    auto updates = s.wipe();
    REQUIRE(updates.size() == 4);
}

TEST_CASE("wipe emits no MarketDataEvent (game-mechanic, not exchange event)") {
    // wipe() return type is vector<BookUpdated>, not ExchangeResult,
    // so by construction no MarketDataEvent can be produced.
    // This test documents the API contract explicitly.
    auto s = make_session();
    s.submit_order(0, Side::Buy, 40, 1);
    std::vector<BookUpdated> updates = s.wipe();  // compiles only if return type is correct
    REQUIRE(updates.size() == 4);
}

// ---------------------------------------------------------------------------
// Sequencer integration
// ---------------------------------------------------------------------------

TEST_CASE("seq stamps are monotonically increasing across submit calls") {
    auto s = make_session();
    auto r1 = s.submit_order(0, Side::Buy, 40, 1);
    auto r2 = s.submit_order(1, Side::Buy, 30, 1);

    auto seq1 = get_market<OrderAdded>(r1).seq;
    auto seq2 = get_market<OrderAdded>(r2).seq;
    REQUIRE(seq2 > seq1);
}

TEST_CASE("seq stamps increase across different event types in one call") {
    // A crossing submit produces OrderAdded (seq N) then OrderExecuted (seq N+1)
    auto s = make_session();
    s.submit_order(0, Side::Sell, 50, 2);
    auto r = s.submit_order(0, Side::Buy, 50, 1);

    auto added_seq = get_market<OrderAdded>(r).seq;
    auto exec_seq  = get_market<OrderExecuted>(r).seq;
    REQUIRE(exec_seq == added_seq + 1);
}

TEST_CASE("reset_seq restarts sequence at 1 on next operation") {
    auto s = make_session();
    s.submit_order(0, Side::Buy, 40, 1);  // uses seq 1
    s.submit_order(0, Side::Buy, 38, 1);  // uses seq 2
    s.reset_seq();

    auto r = s.submit_order(1, Side::Buy, 30, 1);
    REQUIRE(get_market<OrderAdded>(r).seq == 1);
}

// ---------------------------------------------------------------------------
// Book state queries
// ---------------------------------------------------------------------------

TEST_CASE("best_bid returns nullopt on empty book") {
    auto s = make_session();
    REQUIRE_FALSE(s.best_bid(0).has_value());
}

TEST_CASE("best_ask returns nullopt on empty book") {
    auto s = make_session();
    REQUIRE_FALSE(s.best_ask(0).has_value());
}

TEST_CASE("best_bid reflects highest resting buy") {
    auto s = make_session();
    s.submit_order(0, Side::Buy, 40, 1);
    s.submit_order(0, Side::Buy, 45, 2);
    REQUIRE(s.best_bid(0).value() == 45);
}

TEST_CASE("best_ask reflects lowest resting sell") {
    auto s = make_session();
    s.submit_order(0, Side::Sell, 70, 1);
    s.submit_order(0, Side::Sell, 60, 2);
    REQUIRE(s.best_ask(0).value() == 60);
}

TEST_CASE("best_bid_slot returns player with best bid") {
    auto s = make_session();
    s.submit_order(0, Side::Buy, 40, 1);
    s.submit_order(0, Side::Buy, 45, 2);
    REQUIRE(s.best_bid_slot(0).value() == 2);
}

TEST_CASE("best_ask_slot returns player with best ask") {
    auto s = make_session();
    s.submit_order(0, Side::Sell, 70, 1);
    s.submit_order(0, Side::Sell, 60, 2);
    REQUIRE(s.best_ask_slot(0).value() == 2);
}

TEST_CASE("bids_snapshot reflects all resting bids in descending order") {
    auto s = make_session();
    s.submit_order(0, Side::Buy, 40, 1);
    s.submit_order(0, Side::Buy, 45, 2);
    auto snaps = s.bids_snapshot(0);
    REQUIRE(snaps.size() == 2);
    REQUIRE(snaps[0].price == 45);  // best bid first
    REQUIRE(snaps[1].price == 40);
}

TEST_CASE("asks_snapshot reflects all resting asks in ascending order") {
    auto s = make_session();
    s.submit_order(0, Side::Sell, 70, 1);
    s.submit_order(0, Side::Sell, 60, 2);
    auto snaps = s.asks_snapshot(0);
    REQUIRE(snaps.size() == 2);
    REQUIRE(snaps[0].price == 60);  // best ask first
    REQUIRE(snaps[1].price == 70);
}

// ---------------------------------------------------------------------------
// Instrument isolation
// ---------------------------------------------------------------------------

TEST_CASE("exchange maintains separate books per instrument") {
    auto s = make_session();
    s.submit_order(0, Side::Buy, 50, 1);

    REQUIRE(s.best_bid(0).value() == 50);
    REQUIRE_FALSE(s.best_bid(1).has_value());
    REQUIRE_FALSE(s.best_bid(2).has_value());
    REQUIRE_FALSE(s.best_bid(3).has_value());
}

TEST_CASE("instrument_count matches construction") {
    auto s = make_session();
    REQUIRE(s.instrument_count() == 4);
}

TEST_CASE("crossing on one instrument does not affect other instruments") {
    auto s = make_session();
    s.submit_order(0, Side::Sell, 50, 2);
    s.submit_order(1, Side::Buy, 45, 1);

    // cross on instrument 0
    auto r = s.submit_order(0, Side::Buy, 50, 1);
    REQUIRE(has_market<OrderExecuted>(r));

    // instrument 1's order untouched
    REQUIRE(s.best_bid(1).value() == 45);
}
