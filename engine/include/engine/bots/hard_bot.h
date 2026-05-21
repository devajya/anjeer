#pragma once

// Internal header — not exposed via engine.h. Include bot_agent.h for the public interface.

#include "engine/bots/bot_agent.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <random>
#include <vector>

namespace anjeer::engine {

// HardBot: exact Bayesian fill update, book-event update, per-player tracking,
// deterministic lock-in on posterior collapse or card count elimination.
class HardBot : public BotAgent {
public:
    HardBot(const BotConfig& cfg, uint64_t seed) : cfg_(cfg), rng_(seed) {}

    void on_event(const BotEvent& event) override;
    [[nodiscard]] std::vector<BotAction> decide(const GameStateSnapshot& snap) override;
    std::string_view name() const override { return "HardBot"; }
    std::string debug_info(const GameStateSnapshot& snap) const override;

    // Test accessors.
    const std::array<float, 12>& deck_weights()  const { return deck_weights_; }
    std::array<float, 4>         goal_probs()    const { return goal_posteriors(deck_weights_); }
    bool is_locked_in()   const { return goal_suit_locked_; }
    int  locked_goal()    const { return locked_goal_suit_; }
    void set_deck_weights(const std::array<float, 12>& w) { deck_weights_ = w; renormalise(deck_weights_); }

private:
    using clock = std::chrono::steady_clock;

    BotConfig       cfg_;
    std::mt19937_64 rng_;

    std::array<float, 12>  deck_weights_{};
    std::array<int, 4>     hand_{};
    int32_t                balance_      = 0;
    int                    player_slot_  = 0;
    int                    player_count_ = 4;
    bool                   round_active_      = false;
    bool                   has_fill_          = false;
    int                    own_fill_cooldown_ = 0;

    // Lock-in state.
    bool                   goal_suit_locked_   = false;
    int                    locked_goal_suit_   = -1;
    std::array<int, 4>     observed_suit_counts_{};

    // Per-player tracking ([player_slot][suit]).
    std::vector<std::array<int,   4>> player_holdings_;
    std::vector<std::array<float, 4>> player_pressure_;

    // Previous book state for detecting aggressive new orders.
    std::array<std::optional<int32_t>, 4> prev_best_bid_;
    std::array<std::optional<int32_t>, 4> prev_best_ask_;
    std::array<std::optional<int32_t>, 4> last_trade_price_{};

    std::optional<BotPendingOrder> pending_orders_[4][2]{};

    static int side_idx(Side s) { return s == Side::Buy ? 0 : 1; }
    void clear_pending();
    void check_lock_in();
    void trigger_lock_in();

    void handle(const BotRoundStartEvent& e);
    void handle(const BotTradeEvent& e);
    void handle(const BotBookUpdateEvent& e);
    void handle(const BotOrderAckEvent& e);
    void handle(const BotRoundEndEvent&)    { round_active_ = false; goal_suit_locked_ = false; locked_goal_suit_ = -1; }
    void handle(const BotInterRoundEvent&)  { round_active_ = false; }
    template<typename T> void handle(const T&) {}

    float ev(int suit_idx) const;

    std::vector<BotAction> taker_scan(const GameStateSnapshot& snap) const;
    std::vector<BotAction> review_pending(const GameStateSnapshot& snap);
    std::vector<BotAction> gap_fill(const GameStateSnapshot& snap) const;
    std::vector<BotAction> locked_actions(const GameStateSnapshot& snap) const;
    std::vector<BotAction> seed_market(const GameStateSnapshot& snap) const;
};

std::unique_ptr<HardBot> make_hard_bot(const BotConfig& cfg, uint64_t seed);

} // namespace anjeer::engine
