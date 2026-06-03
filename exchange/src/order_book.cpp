#include "exchange/order_book.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace anjeer::exchange {

OrderBook::OrderBook(Config cfg) : cfg_(std::move(cfg)) {}

// Consumes resting orders FIFO until the aggressor's qty reaches zero or no price cross exists.
// Resting orders with remaining qty > 0 stay in the book.
// Returns one TradeEvent per execution; may return zero (no match) or many (partial fills).
std::vector<TradeEvent> OrderBook::match_loop() {
    std::vector<TradeEvent> trades;
    while (!bids_.empty() && !asks_.empty()) {
        Order& bid = bids_.front();
        Order& ask = asks_.front();
        if (bid.price < ask.price) break;
        if (bid.player_id == ask.player_id) break;

        const bool    bid_is_aggressor = bid.id > ask.id;
        const Side    aggressor_side   = bid_is_aggressor ? Side::Buy : Side::Sell;
        const int32_t exec_price       = bid_is_aggressor ? ask.price : bid.price;
        const int32_t fill             = std::min(bid.qty, ask.qty);

        trades.push_back(TradeEvent{
            cfg_.suit, exec_price, bid.player_id, ask.player_id, aggressor_side, fill});

        bid.qty -= fill;
        ask.qty -= fill;

        if (bid.qty == 0) bids_.erase(bids_.begin());
        if (ask.qty == 0) asks_.erase(asks_.begin());
    }
    return trades;
}

bool OrderBook::is_valid_price(int32_t price) const noexcept {
    return price >= cfg_.min_price && price <= cfg_.max_price;
}

BookUpdateEvent OrderBook::make_book_update() const {
    const std::optional<int32_t> bid_pid = bids_.empty() ? std::nullopt : std::optional<int32_t>(bids_.front().player_id);
    const std::optional<int32_t> ask_pid = asks_.empty() ? std::nullopt : std::optional<int32_t>(asks_.front().player_id);
    return BookUpdateEvent{cfg_.suit, best_bid(), best_ask(), bid_pid, ask_pid};
}


std::optional<int32_t> OrderBook::best_bid() const noexcept {
    if (bids_.empty()) return std::nullopt;
    return bids_.front().price;
}

std::optional<int32_t> OrderBook::best_ask() const noexcept {
    if (asks_.empty()) return std::nullopt;
    return asks_.front().price;
}

std::vector<OrderEvent> OrderBook::wipe() {
    bids_.clear();
    asks_.clear();
    return {make_book_update()};
}

std::vector<OrderEvent> OrderBook::submit(int32_t player_id, Side side, int32_t price, int32_t qty) {
    if (!is_valid_price(price)) {
        return {OrderErrorEvent{
            OrderErrorEvent::Code::PriceOutOfRange,
            "price " + std::to_string(price) +
            " out of range [" + std::to_string(cfg_.min_price) +
            ", " + std::to_string(cfg_.max_price) + "]"
        }};
    }

    Order order{next_id_++, player_id, side, price, qty};

    // lower_bound maintains price-time priority (descending price for bids, ascending for asks;
    // ascending id on ties for FIFO within a price level).
    if (side == Side::Buy) {
        const auto it = std::lower_bound(bids_.begin(), bids_.end(), order,
            [](const Order& a, const Order& b) noexcept {
                if (a.price != b.price) return a.price > b.price;
                return a.id < b.id;
            });
        bids_.insert(it, order);
    } else {
        const auto it = std::lower_bound(asks_.begin(), asks_.end(), order,
            [](const Order& a, const Order& b) noexcept {
                if (a.price != b.price) return a.price < b.price;
                return a.id < b.id;
            });
        asks_.insert(it, order);
    }

    std::vector<OrderEvent> events;
    events.reserve(4);
    events.emplace_back(OrderAckEvent{order.id, player_id, side, price, cfg_.suit, qty});

    for (auto& trade : match_loop()) {
        events.emplace_back(std::move(trade));
    }

    // BookUpdateEvent is always last; server reads trade events first, then book state.
    events.emplace_back(make_book_update());
    return events;
}

std::vector<OrderEvent> OrderBook::cancel(int64_t order_id, int32_t player_id) {
    for (auto it = bids_.begin(); it != bids_.end(); ++it) {
        if (it->id != order_id) continue;
        if (it->player_id != player_id) {
            return {OrderErrorEvent{OrderErrorEvent::Code::NotYourOrder,
                "order " + std::to_string(order_id) + " belongs to another player"}};
        }
        bids_.erase(it);
        return {OrderCancelAckEvent{order_id}, make_book_update()};
    }

    for (auto it = asks_.begin(); it != asks_.end(); ++it) {
        if (it->id != order_id) continue;
        if (it->player_id != player_id) {
            return {OrderErrorEvent{OrderErrorEvent::Code::NotYourOrder,
                "order " + std::to_string(order_id) + " belongs to another player"}};
        }
        asks_.erase(it);
        return {OrderCancelAckEvent{order_id}, make_book_update()};
    }

    return {OrderErrorEvent{OrderErrorEvent::Code::OrderNotFound,
        "order " + std::to_string(order_id) + " not found"}};
}

std::vector<OrderEvent> OrderBook::cancel_player(int32_t player_id) {
    // Collect IDs first to avoid iterator invalidation during cancellation.
    std::vector<int64_t> ids;
    for (const auto& o : bids_) if (o.player_id == player_id) ids.push_back(o.id);
    for (const auto& o : asks_) if (o.player_id == player_id) ids.push_back(o.id);
    std::vector<OrderEvent> all;
    for (auto id : ids) {
        auto evs = cancel(id, player_id);
        all.insert(all.end(), evs.begin(), evs.end());
    }
    return all;
}

std::vector<OrderBook::OrderSnapshot> OrderBook::bids_snapshot() const {
    std::vector<OrderSnapshot> snap;
    snap.reserve(bids_.size());
    for (const auto& o : bids_)
        snap.push_back({o.id, o.price, o.player_id});
    return snap;
}

std::vector<OrderBook::OrderSnapshot> OrderBook::asks_snapshot() const {
    std::vector<OrderSnapshot> snap;
    snap.reserve(asks_.size());
    for (const auto& o : asks_)
        snap.push_back({o.id, o.price, o.player_id});
    return snap;
}

} // namespace anjeer::exchange
