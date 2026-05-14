#pragma once

#include "engine/bots/bot_types.h"

#include <memory>
#include <string_view>

namespace anjeer::engine {

class BotAgent {
public:
    virtual ~BotAgent() = default;
    virtual void on_event(const BotEvent& event) = 0;
    // Returns all intended actions for this tick, ranked by urgency (highest first).
    // BotAdapter submits up to max_concurrent_orders actions per tick.
    [[nodiscard]]
    virtual std::vector<BotAction> decide(const BotGameSnapshot& snapshot) = 0;
    virtual std::string_view name() const = 0;
    // Returns a one-line string of internal state for diagnostic logging.
    // Called by BotAdapter after every decide(); no I/O here.
    virtual std::string debug_info(const BotGameSnapshot& snap) const = 0;
};

// Factory — only entry point consumers use.
// seed provides a reproducible RNG for testing.
std::unique_ptr<BotAgent> make_bot(
    BotDifficulty difficulty,
    const BotConfig& config,
    uint32_t seed
);

} // namespace anjeer::engine
