#include "engine/bots/bot_agent.h"
#include "engine/bots/easy_bot.h"
#include "engine/bots/medium_bot.h"
#include "engine/bots/hard_bot.h"

namespace anjeer::engine {

std::unique_ptr<BotAgent> make_bot(
    BotDifficulty difficulty,
    const BotConfig& config,
    uint32_t seed)
{
    switch (difficulty) {
        case BotDifficulty::Easy:   return std::make_unique<EasyBot>(config, seed);
        case BotDifficulty::Medium: return std::make_unique<MediumBot>(config, seed);
        case BotDifficulty::Hard:   return std::make_unique<HardBot>(config, seed);
    }
    return nullptr;
}

} // namespace anjeer::engine
