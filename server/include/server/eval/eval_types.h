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
};

struct EvalBookUpdate {
    engine::Suit           suit;
    std::optional<int32_t> best_bid;
    std::optional<int32_t> best_ask;
    std::optional<int32_t> best_bid_slot;
    std::optional<int32_t> best_ask_slot;
};

struct EvalOutput {
    enum class Type { PosteriorUpdate, AccumulationSignal, ExecutionGuidance };
    Type           type;
    int            target_slot;  // -1 = broadcast (public), 0-3 = private per-slot
    nlohmann::json payload;
};

using EvalOutputCallback = std::function<void(EvalOutput)>;

} // namespace anjeer::server::eval
