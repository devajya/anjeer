#pragma once

// Internal header — not exposed via engine.h. Include bot_agent.h for the public interface.

#include "engine/bots/bot_agent.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <vector>

namespace anjeer::engine {

// EasyBot: hand-heuristic belief (no hypergeometric), no belief updates.
// Error: identifies heaviest held suit as the 12-card suit, then correctly inverts
// to the same-colour partner as the goal — but may have the wrong 12-card suit.
class EasyBot : public BotAgent {
public:
    EasyBot(const BotConfig& cfg, uint64_t seed) : cfg_(cfg), rng_(seed) {}

    void on_event(const BotEvent& event) override;
    [[nodiscard]] std::vector<BotAction> decide(const GameStateSnapshot& snap) override;
    std::string_view name() const override { return "EasyBot"; }
    std::string debug_info(const GameStateSnapshot& snap) const override;

    // Test accessor.
    std::array<float, 4> goal_probs() const { return goal_posteriors(deck_weights_); }

private:
    using clock = std::chrono::steady_clock;

    BotConfig              cfg_;
    std::mt19937_64        rng_;

    std::array<float, 12>  deck_weights_{};
    std::array<int, 4>     hand_{};
    int32_t                balance_      = 0;
    int                    player_count_ = 4;
    bool                   round_active_ = false;

    // Pending orders: [suit_index][0=Buy, 1=Sell]
    std::optional<BotPendingOrder> pending_orders_[4][2]{};

    static int side_idx(Side s) { return s == Side::Buy ? 0 : 1; }
    void clear_pending();

    void handle(const BotRoundStartEvent& e);
    void handle(const BotTradeEvent& e);
    void handle(const BotOrderAckEvent& e);
    void handle(const BotRoundEndEvent&)    { round_active_ = false; }
    void handle(const BotInterRoundEvent&)  { round_active_ = false; }
    template<typename T> void handle(const T&) {}

    float ev(int suit_idx) const;

    std::vector<BotAction> taker_scan(const GameStateSnapshot& snap) const;
    std::vector<BotAction> review_pending(const GameStateSnapshot& snap);
    std::vector<BotAction> gap_fill(const GameStateSnapshot& snap) const;
};

} // namespace anjeer::engine
