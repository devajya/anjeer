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

// Configuration for one tradeable instrument.
// Structurally identical to OrderBook::Config — renamed and promoted to the
// exchange-session boundary so callers (GameSession) do not need to include
// order_book.h directly.
// instrument_id is implied by the vector index passed to ExchangeSession's
// constructor (instruments[0] → instrument_id 0, etc.).
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
        instrument_id_t instrument_id, Side side, price_t price, int32_t player_slot);

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

    // ── Book state queries ────────────────────────────────────────────────
    // Used by GameSession for nudge price computation (Q3 option B resolution:
    // nudge price is computed in GameSession, not exposed as a method here).
    [[nodiscard]] std::optional<price_t> best_bid(instrument_id_t instrument_id) const;
    [[nodiscard]] std::optional<price_t> best_ask(instrument_id_t instrument_id) const;

    // best_bid_slot / best_ask_slot derived from bids_snapshot / asks_snapshot
    // (OrderBook does not expose a direct player-id query for best level).
    [[nodiscard]] std::optional<int32_t> best_bid_slot(instrument_id_t instrument_id) const;
    [[nodiscard]] std::optional<int32_t> best_ask_slot(instrument_id_t instrument_id) const;

    // Full order snapshots for state serialization (reconnect, eval).
    [[nodiscard]] std::vector<OrderBook::OrderSnapshot> bids_snapshot(instrument_id_t instrument_id) const;
    [[nodiscard]] std::vector<OrderBook::OrderSnapshot> asks_snapshot(instrument_id_t instrument_id) const;

    [[nodiscard]] std::size_t instrument_count() const noexcept { return books_.size(); }

private:
    std::vector<OrderBook> books_;
    Sequencer              sequencer_;

    // Translate a raw vector<OrderEvent> from a single OrderBook call into an
    // ExchangeResult, stamping seq numbers on every outbound event. The
    // instrument_id parameter is passed through because OrderBook events carry
    // the suit string label, not the numeric instrument id.
    ExchangeResult translate(instrument_id_t instrument_id, std::vector<OrderEvent> events);

    // Map OrderErrorEvent::Code → OrderRejected::Code. The two enums have
    // identical values but are intentionally separate types (see AGENT-CTX
    // in market_data.h).
    static OrderRejected::Code map_error_code(OrderErrorEvent::Code code) noexcept;
};

} // namespace anjeer::exchange
