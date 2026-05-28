#pragma once

// OrderBook now lives in exchange/. Re-exported here so existing engine consumers
// compile unchanged during the transition.
#include "exchange/order_book.h"

namespace anjeer::engine {
    using Side                = anjeer::exchange::Side;
    using OrderAckEvent       = anjeer::exchange::OrderAckEvent;
    using TradeEvent          = anjeer::exchange::TradeEvent;
    using BookUpdateEvent     = anjeer::exchange::BookUpdateEvent;
    using OrderCancelAckEvent = anjeer::exchange::OrderCancelAckEvent;
    using OrderErrorEvent     = anjeer::exchange::OrderErrorEvent;
    using OrderEvent          = anjeer::exchange::OrderEvent;
    using OrderBook           = anjeer::exchange::OrderBook;
} // namespace anjeer::engine
