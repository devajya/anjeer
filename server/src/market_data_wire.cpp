#include "server/market_data_wire.h"

#include <nlohmann/json.hpp>

using anjeer::engine::Suit;
using anjeer::engine::suit_name;
using anjeer::exchange::order_id_t;
using anjeer::exchange::price_t;
using anjeer::exchange::seq_t;
using anjeer::exchange::Side;
using anjeer::exchange::PriceLevel;
using anjeer::exchange::OrderEntry;

namespace anjeer::server::wire {

namespace {
const char* side_str(Side s) noexcept {
    return s == Side::Buy ? "buy" : "sell";
}
} // namespace

std::string book_update(Suit suit,
                        std::optional<price_t> bid,
                        std::optional<price_t> ask,
                        seq_t seq) {
    return nlohmann::json{
        {"type",     "book_update"},
        {"v",        kSchemaVersion},
        {"seq",      seq},
        {"suit",     suit_name(suit)},
        {"best_bid", bid.has_value() ? nlohmann::json(*bid) : nlohmann::json(nullptr)},
        {"best_ask", ask.has_value() ? nlohmann::json(*ask) : nlohmann::json(nullptr)},
    }.dump();
}

std::string book_depth(Suit suit,
                       const std::vector<PriceLevel>& bids,
                       const std::vector<PriceLevel>& asks,
                       seq_t seq) {
    nlohmann::json bids_arr = nlohmann::json::array();
    for (const auto& lvl : bids)
        bids_arr.push_back({{"price", lvl.price}, {"qty", lvl.qty}});

    nlohmann::json asks_arr = nlohmann::json::array();
    for (const auto& lvl : asks)
        asks_arr.push_back({{"price", lvl.price}, {"qty", lvl.qty}});

    return nlohmann::json{
        {"type", "book_depth"},
        {"v",    kSchemaVersion},
        {"seq",  seq},
        {"suit", suit_name(suit)},
        {"bids", bids_arr},
        {"asks", asks_arr},
    }.dump();
}

std::string book_depth_snapshot(Suit suit,
                                const std::vector<PriceLevel>& bids,
                                const std::vector<PriceLevel>& asks,
                                seq_t seq) {
    nlohmann::json bids_arr = nlohmann::json::array();
    for (const auto& lvl : bids)
        bids_arr.push_back({{"price", lvl.price}, {"qty", lvl.qty}});

    nlohmann::json asks_arr = nlohmann::json::array();
    for (const auto& lvl : asks)
        asks_arr.push_back({{"price", lvl.price}, {"qty", lvl.qty}});

    return nlohmann::json{
        {"type", "book_depth_snapshot"},
        {"v",    kSchemaVersion},
        {"seq",  seq},
        {"suit", suit_name(suit)},
        {"bids", bids_arr},
        {"asks", asks_arr},
    }.dump();
}

std::string order_added(order_id_t order_id,
                        Suit suit,
                        Side side,
                        price_t price,
                        seq_t seq) {
    return nlohmann::json{
        {"type",     "order_added"},
        {"v",        kSchemaVersion},
        {"seq",      seq},
        {"order_id", order_id},
        {"suit",     suit_name(suit)},
        {"side",     side_str(side)},
        {"price",    price},
    }.dump();
}

std::string order_executed(order_id_t order_id,
                           Suit suit,
                           price_t price,
                           Side aggressor,
                           int buyer_slot,
                           int seller_slot,
                           seq_t seq) {
    return nlohmann::json{
        {"type",           "order_executed"},
        {"v",              kSchemaVersion},
        {"seq",            seq},
        {"order_id",       order_id},
        {"suit",           suit_name(suit)},
        {"price",          price},
        {"aggressor_side", side_str(aggressor)},
        {"buyer_slot",     buyer_slot},
        {"seller_slot",    seller_slot},
    }.dump();
}

std::string order_cancelled(order_id_t order_id,
                            Suit suit,
                            seq_t seq) {
    return nlohmann::json{
        {"type",     "order_cancelled"},
        {"v",        kSchemaVersion},
        {"seq",      seq},
        {"order_id", order_id},
        {"suit",     suit_name(suit)},
    }.dump();
}

std::string order_book_snapshot(Suit suit,
                                const std::vector<OrderEntry>& bids,
                                const std::vector<OrderEntry>& asks,
                                seq_t seq) {
    nlohmann::json bids_arr = nlohmann::json::array();
    for (const auto& e : bids)
        bids_arr.push_back({{"order_id", e.order_id}, {"price", e.price}});

    nlohmann::json asks_arr = nlohmann::json::array();
    for (const auto& e : asks)
        asks_arr.push_back({{"order_id", e.order_id}, {"price", e.price}});

    return nlohmann::json{
        {"type", "order_book_snapshot"},
        {"v",    kSchemaVersion},
        {"seq",  seq},
        {"suit", suit_name(suit)},
        {"bids", bids_arr},
        {"asks", asks_arr},
    }.dump();
}

} // namespace anjeer::server::wire
