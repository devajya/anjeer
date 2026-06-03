#pragma once

#include "server/feed_tier.h"

#include <array>
#include <cstdint>
#include <string>

namespace anjeer::server {

// Round-level facts needed to construct or replace a bot.
// Passed to BotAdapter and BotManager::spawn_replacement to avoid long parameter lists.
struct BotSpawnContext {
    int                slot;
    std::array<int, 4> hand;
    int32_t            balance;
    float              remaining_s;
    std::string        difficulty_str; // "easy" | "medium" | "hard" | "random"
    int32_t            points_per_card;
    int32_t            buy_in;
    int                round_duration_s;
    FeedTier           feed = FeedTier::MBP1;
};

} // namespace anjeer::server
