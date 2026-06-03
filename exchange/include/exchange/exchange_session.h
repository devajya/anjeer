#pragma once

#include "exchange/exchange_types.h"
#include "exchange/market_data.h"
#include "exchange/order_book.h"
#include "exchange/sequencer.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace anjeer::exchange {

// Aggregated price level for MBP-N depth view.
struct PriceLevel {
    price_t price;
    int     qty;    // total resting quantity at this price
};

// Single resting order for MBO view.
struct OrderEntry {
    order_id_t order_id;
    price_t    price;
};

// Configuration for one tradeable instrument.
// instrument_id is implied by the position in the vector passed to ExchangeSession
// (instruments[0] → instrument_id 0, etc.).
struct InstrumentConfig {
    price_t     min_price                = 1;
    price_t     max_price                = 99;
    price_t     nudge_initial_buy_price  = 1;
    price_t     nudge_initial_sell_price = 99;
    std::string label;   // e.g. "S1" … "S4" — was "suit" in OrderBook::Config
};

// ---------------------------------------------------------------------------
// ExchangeSession — single game-agnostic matching session.
//
// Owns N OrderBook instances (one per instrument) and a Sequencer.
// Every mutating call returns an ExchangeResult containing:
//   .feedback  — operational events for the orchestration layer (GameSession)
//   .market    — observable feed events for future subscribers (Slice 18/21)
//
// Not thread-safe. All calls must happen on the same thread (the game-loop
// thread in production; the test thread in tests).
//
// One instance per active GameSession — no singleton, no shared state.
// ---------------------------------------------------------------------------
class ExchangeSession {
public:
    explicit ExchangeSession(std::vector<InstrumentConfig> instruments);

    // Submit a new limit order. On success, feedback contains OrderAck and
    // possibly BookUpdated; market contains OrderAdded and possibly OrderExecuted.
    // If the result market contains OrderExecuted, the caller (GameSession) must
    // call wipe() on all instruments — this is the global-wipe game mechanic.
    [[nodiscard]] ExchangeResult submit_order(
        instrument_id_t instrument_id, Side side, price_t price, int32_t player_slot, int32_t qty = 1);

    // Cancel a resting order by id. On success, feedback contains CancelAck and
    // BookUpdated; market contains OrderCancelled. On failure, feedback contains
    // OrderRejected.
    [[nodiscard]] ExchangeResult cancel_order(
        int64_t order_id, instrument_id_t instrument_id, int32_t player_slot);

    // Cancel all resting orders for player_slot across ALL instruments.
    // Accumulates ExchangeResult from each book; returns the merged result.
    // Used by GameSession when a player disconnects or permanently leaves.
    [[nodiscard]] ExchangeResult cancel_player(int32_t player_slot);

    // Clear all books. Returns one BookUpdated per instrument (null bid/ask).
    // Does NOT emit MarketDataEvent — wipe is a game-mechanic, not an exchange
    // event (see AGENT-CTX in market_data.h for rationale).
    [[nodiscard]] std::vector<BookUpdated> wipe();

    // Reset the sequencer counter to 0. Call at begin_round so seq restarts
    // from 1 each round — makes sequence numbers round-relative for consumers.
    void reset_seq();

    // Returns the last seq issued this round, or 0 if no events have been stamped yet.
    [[nodiscard]] seq_t current_seq() const noexcept;

    // ── Book state queries ────────────────────────────────────────────────
    // Used by GameSession for nudge price computation (Q3 option B resolution:
    // nudge price is computed in GameSession, not exposed as a method here).
    [[nodiscard]] std::optional<price_t> best_bid(instrument_id_t instrument_id) const;
    [[nodiscard]] std::optional<price_t> best_ask(instrument_id_t instrument_id) const;

    [[nodiscard]] std::optional<int32_t> best_bid_slot(instrument_id_t instrument_id) const;
    [[nodiscard]] std::optional<int32_t> best_ask_slot(instrument_id_t instrument_id) const;

    // Full order snapshots for state serialization (reconnect, eval).
    [[nodiscard]] std::vector<OrderBook::OrderSnapshot> bids_snapshot(instrument_id_t instrument_id) const;
    [[nodiscard]] std::vector<OrderBook::OrderSnapshot> asks_snapshot(instrument_id_t instrument_id) const;

    // MBP-N: price-level aggregated depth view (qty = total resting qty at level).
    // Returns at most `depth` levels ordered best-price first.
    [[nodiscard]] std::vector<PriceLevel> bids_depth(instrument_id_t instrument_id, int depth) const;
    [[nodiscard]] std::vector<PriceLevel> asks_depth(instrument_id_t instrument_id, int depth) const;

    // MBO: individual order view (one entry per resting order, best-price first).
    [[nodiscard]] std::vector<OrderEntry> bids_mbo(instrument_id_t instrument_id) const;
    [[nodiscard]] std::vector<OrderEntry> asks_mbo(instrument_id_t instrument_id) const;

    [[nodiscard]] std::size_t instrument_count() const noexcept { return books_.size(); }

private:
    std::vector<OrderBook> books_;
    Sequencer              sequencer_;

    ExchangeResult translate(instrument_id_t instrument_id, std::vector<OrderEvent> events);
    static OrderRejected::Code map_error_code(OrderErrorEvent::Code code) noexcept;
};

} // namespace anjeer::exchange
