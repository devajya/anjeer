#pragma once

#include "exchange/exchange_session.h"
#include "exchange/exchange_types.h"
#include "engine/suit.h"

#include <optional>
#include <string>
#include <vector>

namespace anjeer::server::wire {

static constexpr int kSchemaVersion = 1;

// MBP-1: top-of-book update (extends legacy book_update with v + seq)
std::string book_update(anjeer::engine::Suit suit,
                        std::optional<anjeer::exchange::price_t> bid,
                        std::optional<anjeer::exchange::price_t> ask,
                        anjeer::exchange::seq_t seq);

// MBP-N: incremental depth update
std::string book_depth(anjeer::engine::Suit suit,
                       const std::vector<anjeer::exchange::PriceLevel>& bids,
                       const std::vector<anjeer::exchange::PriceLevel>& asks,
                       anjeer::exchange::seq_t seq);

// MBP-N: full depth snapshot
std::string book_depth_snapshot(anjeer::engine::Suit suit,
                                const std::vector<anjeer::exchange::PriceLevel>& bids,
                                const std::vector<anjeer::exchange::PriceLevel>& asks,
                                anjeer::exchange::seq_t seq);

// MBO: incremental — new resting order
std::string order_added(anjeer::exchange::order_id_t order_id,
                        anjeer::engine::Suit suit,
                        anjeer::exchange::Side side,
                        anjeer::exchange::price_t price,
                        anjeer::exchange::seq_t seq);

// MBO: incremental — fill
std::string order_executed(anjeer::exchange::order_id_t order_id,
                           anjeer::engine::Suit suit,
                           anjeer::exchange::price_t price,
                           anjeer::exchange::Side aggressor,
                           int buyer_slot,
                           int seller_slot,
                           anjeer::exchange::seq_t seq);

// MBO: incremental — cancel
std::string order_cancelled(anjeer::exchange::order_id_t order_id,
                            anjeer::engine::Suit suit,
                            anjeer::exchange::seq_t seq);

// MBO: full order-book snapshot
std::string order_book_snapshot(anjeer::engine::Suit suit,
                                const std::vector<anjeer::exchange::OrderEntry>& bids,
                                const std::vector<anjeer::exchange::OrderEntry>& asks,
                                anjeer::exchange::seq_t seq);

} // namespace anjeer::server::wire
