#include "engine/order_book.h"

#include <algorithm>
#include <stdexcept>
#include <string>

// AGENT-CTX: This is the only translation unit that implements OrderBook.
// Keep it free of network, JSON, and I/O code — all of that belongs in server/.
// The engine must remain testable and threadable in isolation.

namespace anjeer::engine {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

OrderBook::OrderBook(Config cfg) : cfg_(std::move(cfg)) {
    // AGENT-CTX: Vectors start empty; no pre-allocation here because at game
    // scale (≤5 players, ≤1 order per player per suit) the working set is tiny.
    // If a future slice increases player count significantly, add
    //   bids_.reserve(expected_depth);  asks_.reserve(expected_depth);
    // here using the new config value.
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool OrderBook::is_valid_price(int32_t price) const noexcept {
    return price >= cfg_.min_price && price <= cfg_.max_price;
}

BookUpdateEvent OrderBook::make_book_update() const {
    // AGENT-CTX: Returns a snapshot of best bid/ask AFTER the mutation that
    // caused this call. The server broadcasts this immediately — callers must
    // not cache this value across mutations.
    const std::optional<int32_t> bid_pid = bids_.empty() ? std::nullopt : std::optional<int32_t>(bids_.front().player_id);
    const std::optional<int32_t> ask_pid = asks_.empty() ? std::nullopt : std::optional<int32_t>(asks_.front().player_id);
    return BookUpdateEvent{cfg_.suit, best_bid(), best_ask(), bid_pid, ask_pid};
}

std::optional<TradeEvent> OrderBook::try_match() {
    if (bids_.empty() || asks_.empty()) return std::nullopt;

    // AGENT-CTX: Invariant: no crossed book at rest. After every mutation the
    // book is left uncrossed. try_match() is called immediately after insertion
    // so at most one match can occur per call — we do not loop.
    if (bids_.front().price < asks_.front().price) return std::nullopt;

    const Order& bid = bids_.front();
    const Order& ask = asks_.front();

    // AGENT-CTX: Execution price is the MAKER'S (resting order's) price.
    // The maker is the order with the LOWER id (submitted earlier, was resting).
    // The aggressor is the order with the HIGHER id (just submitted, caused the cross).
    // This invariant holds because: before the new order was inserted, the book
    // was uncrossed; the new order always receives the highest id. Therefore the
    // matched order with the higher id is always the newly submitted one.
    // If this logic ever changes (e.g. batch order submission), revisit.
    const bool bid_is_aggressor = bid.id > ask.id;
    const Side aggressor_side   = bid_is_aggressor ? Side::Buy : Side::Sell;
    // Maker's price: the side that was resting set the price
    const int32_t exec_price    = bid_is_aggressor ? ask.price : bid.price;

    TradeEvent ev{
        cfg_.suit,
        exec_price,
        bid.player_id,
        ask.player_id,
        aggressor_side
    };

    // AGENT-CTX: erase(begin()) is O(n) on a vector. At game scale (n≤5) this
    // is faster than any tree structure due to cache locality. See header for
    // the reasoning on why std::vector is the correct container here.
    bids_.erase(bids_.begin());
    asks_.erase(asks_.begin());

    return ev;
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

std::optional<int32_t> OrderBook::best_bid() const noexcept {
    if (bids_.empty()) return std::nullopt;
    return bids_.front().price;
}

std::optional<int32_t> OrderBook::best_ask() const noexcept {
    if (asks_.empty()) return std::nullopt;
    return asks_.front().price;
}

std::vector<OrderEvent> OrderBook::wipe() {
    // AGENT-CTX: Called by the server on every active book after a TradeEvent.
    // Clears all resting orders then returns a BookUpdateEvent with nullopt
    // best_bid / best_ask so the server can broadcast the empty-book state
    // without constructing it independently. See WIPE CONTRACT in order_book.h.
    bids_.clear();
    asks_.clear();
    return {make_book_update()};  // best_bid() / best_ask() are now nullopt
}

std::vector<OrderEvent> OrderBook::submit(int32_t player_id, Side side, int32_t price) {
    if (!is_valid_price(price)) {
        return {OrderErrorEvent{
            OrderErrorEvent::Code::PriceOutOfRange,
            "price " + std::to_string(price) +
            " out of range [" + std::to_string(cfg_.min_price) +
            ", " + std::to_string(cfg_.max_price) + "]"
        }};
    }

    Order order{next_id_++, player_id, side, price};

    // AGENT-CTX: Insert into the sorted vector using lower_bound to maintain
    // price-time priority without a full sort on each insert.
    //
    // Bids: descending price, ascending id on price ties (FIFO within price level).
    //   Comparator: a < b iff a.price > b.price, or same price and a.id < b.id.
    //
    // Asks: ascending price, ascending id on price ties.
    //   Comparator: a < b iff a.price < b.price, or same price and a.id < b.id.
    //
    // AGENT-CTX: lower_bound finds the first position where !cmp(existing, new),
    // i.e. the first position where the new order should be inserted to maintain
    // sorted order. Inserting here is correct for FIFO tie-breaking because
    // existing orders at the same price have lower ids and therefore sort before
    // the new order — the new order goes after all same-price peers.
    if (side == Side::Buy) {
        const auto it = std::lower_bound(bids_.begin(), bids_.end(), order,
            [](const Order& a, const Order& b) noexcept {
                if (a.price != b.price) return a.price > b.price;  // higher price first
                return a.id < b.id;                                 // earlier id first
            });
        bids_.insert(it, order);
    } else {
        const auto it = std::lower_bound(asks_.begin(), asks_.end(), order,
            [](const Order& a, const Order& b) noexcept {
                if (a.price != b.price) return a.price < b.price;  // lower price first
                return a.id < b.id;                                 // earlier id first
            });
        asks_.insert(it, order);
    }

    // AGENT-CTX: Reserve 3 slots upfront (OrderAck + optional TradeEvent +
    // BookUpdate) to avoid reallocation on the common path. Per cpp_performance_rules:
    // "reserve vector capacity upfront to avoid repeated reallocations".
    std::vector<OrderEvent> events;
    events.reserve(3);
    events.emplace_back(OrderAckEvent{order.id, player_id, side, price, cfg_.suit});

    if (auto trade = try_match()) {
        events.emplace_back(std::move(*trade));
    }

    // AGENT-CTX: BookUpdateEvent is ALWAYS the last event in the result vector.
    // The server relies on this ordering: it reads trade events first (to perform
    // the wipe), then reads the book update (to broadcast the current state).
    // Do not reorder these emplace_backs.
    events.emplace_back(make_book_update());
    return events;
}

std::vector<OrderEvent> OrderBook::cancel(int64_t order_id, int32_t player_id) {
    // AGENT-CTX: Linear scan over bids_ then asks_. Correct and fast at game
    // scale (n≤5). If depth grows to hundreds of orders, consider an auxiliary
    // unordered_map<int64_t, iterator> for O(1) lookup — but profile first.
    for (auto it = bids_.begin(); it != bids_.end(); ++it) {
        if (it->id != order_id) continue;
        if (it->player_id != player_id) {
            return {OrderErrorEvent{
                OrderErrorEvent::Code::NotYourOrder,
                "order " + std::to_string(order_id) + " belongs to another player"
            }};
        }
        bids_.erase(it);
        return {OrderCancelAckEvent{order_id}, make_book_update()};
    }

    for (auto it = asks_.begin(); it != asks_.end(); ++it) {
        if (it->id != order_id) continue;
        if (it->player_id != player_id) {
            return {OrderErrorEvent{
                OrderErrorEvent::Code::NotYourOrder,
                "order " + std::to_string(order_id) + " belongs to another player"
            }};
        }
        asks_.erase(it);
        return {OrderCancelAckEvent{order_id}, make_book_update()};
    }

    return {OrderErrorEvent{
        OrderErrorEvent::Code::OrderNotFound,
        "order " + std::to_string(order_id) + " not found"
    }};
}

// Collect IDs first so iterator invalidation from cancel() calls is not an issue.
std::vector<OrderEvent> OrderBook::cancel_player(int32_t player_id) {
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

std::vector<OrderEvent> OrderBook::nudge(Side side, int32_t player_id) {
    // AGENT-CTX: Nudge always creates a NEW order (Option B per Slice 2 resolution).
    // It does NOT reference or modify any existing order. The computed price is
    // clamped to [min_price, max_price] so a nudge at the ceiling/floor always
    // succeeds rather than producing an error.
    int32_t new_price;
    if (side == Side::Buy) {
        // AGENT-CTX: If no bids exist, start at nudge_initial_buy_price (from config,
        // default 1). This handles the "nudge into an empty book" edge case identified
        // in the Slice 2 resolution (Option B rationale).
        new_price = best_bid().value_or(cfg_.nudge_initial_buy_price - 1) + 1;
        new_price = std::clamp(new_price, cfg_.min_price, cfg_.max_price);
    } else {
        new_price = best_ask().value_or(cfg_.nudge_initial_sell_price + 1) - 1;
        new_price = std::clamp(new_price, cfg_.min_price, cfg_.max_price);
    }

    // AGENT-CTX: Delegate to submit() — nudge is semantically a submit with a
    // computed price. This ensures all validation, insertion, matching, and event
    // production follow the same path as a regular order submission.
    return submit(player_id, side, new_price);
}

} // namespace anjeer::engine
