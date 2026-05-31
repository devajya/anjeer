#pragma once

#include "exchange/exchange_types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace anjeer::exchange {

// ---------------------------------------------------------------------------
// Observable feed — MarketDataEvent
//
// These three variants form the canonical public stream consumed by:
//   Slice 18 — live MBO feed
//   Slice 21 — event recorder
// No parallel event format may be defined elsewhere for these use cases.
//
// All variants carry:
//   seq  — monotonically increasing per-round sequence number stamped by
//           ExchangeSession::translate() before the event leaves the session.
//   v    — format version, always 1 in this slice. Bump when the shape changes
//           so feed consumers can gate on the version rather than the field set.
// ---------------------------------------------------------------------------

// Emitted when a new resting order is placed (non-crossing submit).
struct OrderAdded {
    int64_t         order_id;
    instrument_id_t instrument_id;
    Side            side;
    price_t         price;
    int32_t         player_slot;
    seq_t           seq;
    uint8_t         v = 1;
};

// Emitted when a crossing submit causes a fill.
// order_id == seq in v1; do not assume order_id is the resting order's id.
struct OrderExecuted {
    int64_t         order_id;
    instrument_id_t instrument_id;
    price_t         price;
    Side            aggressor_side;
    int32_t         buyer_slot;
    int32_t         seller_slot;
    seq_t           seq;
    uint8_t         v = 1;
};

// Emitted when a resting order is explicitly cancelled (not from a wipe).
// wipe() does NOT emit OrderCancelled — it returns BookUpdated events only.
// Wipe is a game-mechanic; feed consumers do not observe it as individual cancels.
struct OrderCancelled {
    int64_t         order_id;
    instrument_id_t instrument_id;
    seq_t           seq;
    uint8_t         v = 1;
};

using MarketDataEvent = std::variant<OrderAdded, OrderExecuted, OrderCancelled>;

// ---------------------------------------------------------------------------
// Operational feedback — ExchangeFeedback
//
// Returned alongside MarketDataEvent in ExchangeResult. Consumed only by
// GameSession — NOT part of the observable feed and never forwarded to
// Slice 18/21 consumers.
// ---------------------------------------------------------------------------

// Private confirmation sent to the submitting player only.
struct OrderAck {
    int64_t         order_id;
    instrument_id_t instrument_id;
    Side            side;
    price_t         price;
    int32_t         player_slot;
};

// Broadcast to all clients after any mutation that changes best bid or ask.
// best_bid / best_ask are nullopt when no orders exist on that side.
struct BookUpdated {
    instrument_id_t        instrument_id;
    std::optional<price_t> best_bid;
    std::optional<price_t> best_ask;
    std::optional<int32_t> best_bid_slot;
    std::optional<int32_t> best_ask_slot;
};

// Private error feedback sent to the submitting or cancelling player only.
// Code is distinct from OrderErrorEvent::Code (exchange-boundary vs. internal OrderBook
// vocabulary) so translate() maps between them and market_data.h stays self-contained.
struct OrderRejected {
    enum class Code {
        PriceOutOfRange,
        OrderNotFound,
        NotYourOrder,
        SelfTrade,
        InvalidInstrument,
    };
    Code        code;
    std::string message;
};

// Private cancel confirmation sent to the cancelling player only.
struct CancelAck {
    int64_t         order_id;
    instrument_id_t instrument_id;
};

using ExchangeFeedback = std::variant<OrderAck, BookUpdated, OrderRejected, CancelAck>;

// ---------------------------------------------------------------------------
// ExchangeResult — dual-return from every ExchangeSession mutating operation.
//
// feedback: operational events for GameSession (acks, errors, book updates).
//           GameSession translates these into existing wire-protocol messages.
// market:   observable feed events for future subscribers (Slice 18/21).
//           GameSession also reads market to detect trades (OrderExecuted
//           presence triggers global wipe + card transfers).
// ---------------------------------------------------------------------------
struct ExchangeResult {
    std::vector<ExchangeFeedback> feedback;
    std::vector<MarketDataEvent>  market;
};

} // namespace anjeer::exchange
