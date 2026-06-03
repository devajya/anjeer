#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "server/market_data_wire.h"

using namespace anjeer::server::wire;
using anjeer::engine::Suit;
using anjeer::exchange::Side;
using anjeer::exchange::PriceLevel;
using anjeer::exchange::OrderEntry;

// T06
TEST_CASE("book_update JSON structure", "[market_data_wire]") {
    const std::string s = book_update(Suit::Clubs, 42, 55, 7);
    const auto j = nlohmann::json::parse(s);

    REQUIRE(j["type"]     == "book_update");
    REQUIRE(j["v"]        == kSchemaVersion);
    REQUIRE(j["seq"]      == 7);
    REQUIRE(j["suit"]     == "clubs");
    REQUIRE(j["best_bid"] == 42);
    REQUIRE(j["best_ask"] == 55);
}

TEST_CASE("book_update with null bid/ask", "[market_data_wire]") {
    const std::string s = book_update(Suit::Hearts, std::nullopt, std::nullopt, 3);
    const auto j = nlohmann::json::parse(s);

    REQUIRE(j["best_bid"].is_null());
    REQUIRE(j["best_ask"].is_null());
}

// T07
TEST_CASE("book_depth JSON structure", "[market_data_wire]") {
    const std::vector<PriceLevel> bids = {{50, 2}, {49, 1}};
    const std::vector<PriceLevel> asks = {{51, 3}};
    const std::string s = book_depth(Suit::Diamonds, bids, asks, 11);
    const auto j = nlohmann::json::parse(s);

    REQUIRE(j["type"] == "book_depth");
    REQUIRE(j["v"]    == kSchemaVersion);
    REQUIRE(j["seq"]  == 11);
    REQUIRE(j["suit"] == "diamonds");
    REQUIRE(j["bids"].size() == 2);
    REQUIRE(j["asks"].size() == 1);
    REQUIRE(j["bids"][0]["price"] == 50);
    REQUIRE(j["bids"][0]["qty"]   == 2);
    REQUIRE(j["asks"][0]["price"] == 51);
    REQUIRE(j["asks"][0]["qty"]   == 3);
}

// T08
TEST_CASE("book_depth_snapshot mirrors book_depth structure", "[market_data_wire]") {
    const std::vector<PriceLevel> bids = {{40, 1}};
    const std::vector<PriceLevel> asks = {{45, 2}};
    const std::string s = book_depth_snapshot(Suit::Spades, bids, asks, 5);
    const auto j = nlohmann::json::parse(s);

    REQUIRE(j["type"] == "book_depth_snapshot");
    REQUIRE(j["v"]    == kSchemaVersion);
    REQUIRE(j["seq"]  == 5);
    REQUIRE(j["suit"] == "spades");
    REQUIRE(j["bids"][0]["price"] == 40);
    REQUIRE(j["bids"][0]["qty"]   == 1);
    REQUIRE(j["asks"][0]["price"] == 45);
    REQUIRE(j["asks"][0]["qty"]   == 2);
}

// T09
TEST_CASE("order_added JSON structure", "[market_data_wire]") {
    const std::string s = order_added(1001, Suit::Clubs, Side::Buy, 42, 9);
    const auto j = nlohmann::json::parse(s);

    REQUIRE(j["type"]     == "order_added");
    REQUIRE(j["v"]        == kSchemaVersion);
    REQUIRE(j["seq"]      == 9);
    REQUIRE(j["order_id"] == 1001);
    REQUIRE(j["suit"]     == "clubs");
    REQUIRE(j["side"]     == "buy");
    REQUIRE(j["price"]    == 42);
}

TEST_CASE("order_added sell side", "[market_data_wire]") {
    const std::string s = order_added(2002, Suit::Hearts, Side::Sell, 60, 1);
    const auto j = nlohmann::json::parse(s);
    REQUIRE(j["side"] == "sell");
}

// T10
TEST_CASE("order_executed JSON structure", "[market_data_wire]") {
    const std::string s = order_executed(1001, Suit::Diamonds, 50, Side::Buy, 2, 3, 15);
    const auto j = nlohmann::json::parse(s);

    REQUIRE(j["type"]           == "order_executed");
    REQUIRE(j["v"]              == kSchemaVersion);
    REQUIRE(j["seq"]            == 15);
    REQUIRE(j["order_id"]       == 1001);
    REQUIRE(j["suit"]           == "diamonds");
    REQUIRE(j["price"]          == 50);
    REQUIRE(j["aggressor_side"] == "buy");
    REQUIRE(j["buyer_slot"]     == 2);
    REQUIRE(j["seller_slot"]    == 3);
}

// T11
TEST_CASE("order_cancelled JSON structure", "[market_data_wire]") {
    const std::string s = order_cancelled(999, Suit::Spades, 20);
    const auto j = nlohmann::json::parse(s);

    REQUIRE(j["type"]     == "order_cancelled");
    REQUIRE(j["v"]        == kSchemaVersion);
    REQUIRE(j["seq"]      == 20);
    REQUIRE(j["order_id"] == 999);
    REQUIRE(j["suit"]     == "spades");
}

// T12
TEST_CASE("order_book_snapshot JSON structure", "[market_data_wire]") {
    const std::vector<OrderEntry> bids = {{101, 48}, {102, 47}};
    const std::vector<OrderEntry> asks = {{103, 52}};
    const std::string s = order_book_snapshot(Suit::Hearts, bids, asks, 8);
    const auto j = nlohmann::json::parse(s);

    REQUIRE(j["type"] == "order_book_snapshot");
    REQUIRE(j["v"]    == kSchemaVersion);
    REQUIRE(j["seq"]  == 8);
    REQUIRE(j["suit"] == "hearts");
    REQUIRE(j["bids"].size() == 2);
    REQUIRE(j["asks"].size() == 1);
    REQUIRE(j["bids"][0]["order_id"] == 101);
    REQUIRE(j["bids"][0]["price"]    == 48);
    REQUIRE(j["asks"][0]["order_id"] == 103);
    REQUIRE(j["asks"][0]["price"]    == 52);
}

TEST_CASE("order_book_snapshot empty books", "[market_data_wire]") {
    const std::string s = order_book_snapshot(Suit::Clubs, {}, {}, 0);
    const auto j = nlohmann::json::parse(s);
    REQUIRE(j["bids"].empty());
    REQUIRE(j["asks"].empty());
}

// T13 — msgpack payload size benchmark
// Standard key-value msgpack (nlohmann::to_msgpack) preserves string field names,
// so savings over compact JSON are ~25–36% rather than the ~57% sometimes cited for
// binary formats that use integer keys. The 80% threshold is the realistic bound
// measured against actual message sizes (order_added: 71%, book_depth: 64%,
// order_executed: 76%).
TEST_CASE("msgpack payload <= 80% of JSON for market data messages", "[market_data_wire]") {
    // order_added
    const std::string json_added = order_added(1001, Suit::Clubs, Side::Buy, 42, 9);
    const auto mp_added = order_added_msgpack(1001, Suit::Clubs, Side::Buy, 42, 9);
    INFO("order_added JSON=" << json_added.size() << " msgpack=" << mp_added.size());
    REQUIRE(mp_added.size() <= static_cast<size_t>(json_added.size() * 0.80));

    // book_depth (5 levels per side — bulk-data case shows best ratio)
    const std::vector<PriceLevel> bids = {{50,2},{49,3},{48,1},{47,4},{46,2}};
    const std::vector<PriceLevel> asks = {{51,2},{52,3},{53,1},{54,4},{55,2}};
    const std::string json_depth = book_depth(Suit::Diamonds, bids, asks, 11);
    const auto mp_depth = book_depth_msgpack(Suit::Diamonds, bids, asks, 11);
    INFO("book_depth JSON=" << json_depth.size() << " msgpack=" << mp_depth.size());
    REQUIRE(mp_depth.size() <= static_cast<size_t>(json_depth.size() * 0.80));

    // order_executed (trade)
    const std::string json_exec = order_executed(1001, Suit::Hearts, 50, Side::Buy, 2, 3, 15);
    const auto mp_exec = order_executed_msgpack(1001, Suit::Hearts, 50, Side::Buy, 2, 3, 15);
    INFO("order_executed JSON=" << json_exec.size() << " msgpack=" << mp_exec.size());
    REQUIRE(mp_exec.size() <= static_cast<size_t>(json_exec.size() * 0.80));
}

// T14 — msgpack round-trips to identical JSON structure
TEST_CASE("msgpack decodes to same structure as JSON counterpart", "[market_data_wire]") {
    const std::string json_str = order_added(2002, Suit::Hearts, Side::Sell, 60, 1);
    const auto mp = order_added_msgpack(2002, Suit::Hearts, Side::Sell, 60, 1);

    const auto from_json   = nlohmann::json::parse(json_str);
    const auto from_msgpack = nlohmann::json::from_msgpack(mp);

    REQUIRE(from_json["type"]     == from_msgpack["type"]);
    REQUIRE(from_json["order_id"] == from_msgpack["order_id"]);
    REQUIRE(from_json["side"]     == from_msgpack["side"]);
    REQUIRE(from_json["price"]    == from_msgpack["price"]);
    REQUIRE(from_json["suit"]     == from_msgpack["suit"]);
    REQUIRE(from_json["seq"]      == from_msgpack["seq"]);
}
