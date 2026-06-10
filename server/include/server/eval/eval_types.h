#pragma once

#include "engine/game_snapshot.h"
#include "engine/suit.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>

namespace anjeer::server::eval {

struct EvalTradeEvent {
    int32_t      buyer_slot;
    int32_t      seller_slot;
    int32_t      price;
    engine::Suit suit;
    int64_t      timestamp_ms;
    int32_t      qty{1};
};

struct EvalBookUpdate {
    engine::Suit           suit;
    std::optional<int32_t> best_bid;
    std::optional<int32_t> best_ask;
    std::optional<int32_t> best_bid_slot;
    std::optional<int32_t> best_ask_slot;
};

// Emitted when a new resting order is placed (non-crossing submit).
struct EvalOrderAdded {
    engine::Suit suit;
    int32_t      price;
    int32_t      qty;
    int32_t      slot;
    int64_t      seq;
    int64_t      order_id{-1};
    bool         is_bid{false};
};

// Emitted when a resting order is explicitly cancelled (not a wipe).
// slot is -1 when the placing player cannot be attributed from the market stream.
struct EvalOrderCancelled {
    engine::Suit suit;
    int64_t      order_id;
    int64_t      seq;
};

struct EvalOutput {
    enum class Type { PosteriorUpdate, AccumulationSignal, ExecutionGuidance };
    Type           type;
    int            target_slot;  // -1 = broadcast (public), 0-3 = private per-slot
    nlohmann::json payload;
};

using EvalOutputCallback = std::function<void(EvalOutput)>;

} // namespace anjeer::server::eval
