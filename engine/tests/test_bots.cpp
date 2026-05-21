#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "engine/bots/bot_agent.h"
#include "engine/bots/easy_bot.h"
#include "engine/bots/medium_bot.h"
#include "engine/bots/hard_bot.h"

using namespace anjeer::engine;

// ── Fixtures ──────────────────────────────────────────────────────────────────

static constexpr uint32_t kSeed = 42;

static BotConfig cfg_default() {
    BotConfig c;
    c.confidence_discount   = 0.90f;
    c.taker_threshold       = 0.95f;
    c.min_bid_ev            = 2.0f;
    c.max_ask_ev            = 8.0f;
    c.hand_size_cap         = 5;
    c.offload_threshold     = 4;
    c.max_concurrent_orders = 4;
    c.conviction_threshold  = 0.70f;
    c.max_resting_ms        = 3000;
    c.nudge_probability     = 0.0f;
    c.nudge_patience_ms     = 1500;
    c.nudge_max_gap         = 2;
    c.endgame_threshold_s   = 0;
    c.early_seed_threshold  = 0.0f;
    return c;
}

static GameStateSnapshot make_snapshot(
    const std::array<int, 4>& hand,
    float time_remaining,
    int player_slot  = 0,
    int player_count = 4,
    int32_t balance  = 500)
{
    GameStateSnapshot s;
    s.hand             = hand;
    s.time_remaining_s = time_remaining;
    s.round_duration_s = 240.0f;
    s.my_slot          = player_slot;
    s.num_active_slots = player_count;
    s.points_per_card  = 10;
    s.round_active     = true;
    s.balance          = balance;
    return s;
}

static BotRoundStartEvent make_round_start(
    const std::array<int, 4>& hand,
    float duration   = 240.0f,
    int player_slot  = 0,
    int player_count = 4)
{
    BotRoundStartEvent e;
    e.hand             = hand;
    e.round_duration_s = duration;
    e.player_slot      = player_slot;
    e.player_count     = player_count;
    e.balance          = 500;
    e.buy_in           = 50;
    e.points_per_card  = 10;
    return e;
}

// ── T1: posterior math ────────────────────────────────────────────────────────

TEST_CASE("Posterior: heavy Spades hand gives highest P(goal=Clubs)", "[bots][posterior]") {
    // S=5 most held → S=12-card deck most likely → goal=Clubs (black partner of Spades)
    std::array<int,4> hand = {0, 2, 3, 5};
    auto w = compute_hand_posterior(hand);
    auto P = goal_posteriors(w);
    REQUIRE(P[0] > P[1]);
    REQUIRE(P[0] > P[2]);
    REQUIRE(P[0] > P[3]);
}

TEST_CASE("Posterior: goal_posteriors sums to 1.0", "[bots][posterior]") {
    std::array<int,4> hand = {0, 2, 3, 5};
    auto w = compute_hand_posterior(hand);
    auto P = goal_posteriors(w);
    float sum = P[0] + P[1] + P[2] + P[3];
    REQUIRE(sum == Catch::Approx(1.0f).margin(0.001f));
}

TEST_CASE("Posterior: impossible hand falls back to uniform", "[bots][posterior]") {
    // {10,10,10,10}: every deck has one 8-card suit → C(8,10)=0 → all likelihoods 0 → uniform
    std::array<int,4> balanced = {10, 10, 10, 10};
    auto w = compute_hand_posterior(balanced);
    auto P = goal_posteriors(w);
    for (int s = 0; s < 4; ++s)
        REQUIRE(P[s] == Catch::Approx(0.25f).margin(0.02f));
}

// ── T2: EasyBot ───────────────────────────────────────────────────────────────

TEST_CASE("EasyBot: perceives goal as same-colour partner of heaviest held suit", "[bots][easy]") {
    // hand {0,0,0,7}: heaviest = Spades(3), partner = Clubs(0) → P(goal=Clubs) = 0.80
    std::array<int,4> hand = {0, 0, 0, 7};
    EasyBot bot(cfg_default(), kSeed);
    bot.on_event(make_round_start(hand));

    auto P = bot.goal_probs();
    REQUIRE(P[0] > P[1]);
    REQUIRE(P[0] > P[2]);
    REQUIRE(P[0] > P[3]);
    REQUIRE(P[0] == Catch::Approx(0.80f).margin(0.01f));
}

TEST_CASE("EasyBot: decide() returns vector with prices in [1,20]", "[bots][easy]") {
    std::array<int,4> hand = {2, 2, 3, 3};
    EasyBot bot(cfg_default(), kSeed);
    bot.on_event(make_round_start(hand));

    auto actions = bot.decide(make_snapshot(hand, 120.0f));
    for (const auto& a : actions) {
        if (const auto* s = std::get_if<BotSubmitOrder>(&a)) {
            REQUIRE(s->price >= 1);
            REQUIRE(s->price <= 20);
        }
    }
}

TEST_CASE("EasyBot: no actions when round is inactive", "[bots][easy]") {
    std::array<int,4> hand = {2, 2, 3, 3};
    EasyBot bot(cfg_default(), kSeed);
    // No round_start sent → round_active_ = false
    auto snap = make_snapshot(hand, 120.0f);
    snap.round_active = false;
    auto actions = bot.decide(snap);
    REQUIRE(actions.empty());
}

// ── T3: MediumBot ─────────────────────────────────────────────────────────────

TEST_CASE("MediumBot: balanced hand fallback gives uniform posterior", "[bots][medium]") {
    std::array<int,4> balanced = {10, 10, 10, 10};
    auto bot = make_medium_bot(cfg_default(), kSeed);
    bot->on_event(make_round_start(balanced));

    auto P = bot->goal_probs();
    for (int s = 0; s < 4; ++s)
        REQUIRE(P[s] == Catch::Approx(0.25f).margin(0.02f));
}

TEST_CASE("MediumBot: heavy Spades hand gives highest P(goal=Clubs)", "[bots][medium]") {
    std::array<int,4> hand = {0, 2, 3, 5};
    auto bot = make_medium_bot(cfg_default(), kSeed);
    bot->on_event(make_round_start(hand));

    auto P = bot->goal_probs();
    REQUIRE(P[0] > P[1]);
    REQUIRE(P[0] > P[2]);
    REQUIRE(P[0] > P[3]);
}

TEST_CASE("MediumBot: high-priced Clubs fill increases P(goal=Clubs)", "[bots][medium]") {
    std::array<int,4> hand = {2, 3, 2, 3};
    auto bot = make_medium_bot(cfg_default(), kSeed);
    bot->on_event(make_round_start(hand));

    auto P_before = bot->goal_probs();
    // High price on Clubs → evidence that Clubs is the goal suit
    bot->on_event(BotTradeEvent{Suit::Clubs, 8, std::nullopt});
    auto P_after = bot->goal_probs();

    REQUIRE(P_after[0] > P_before[0]);
}

// ── T4: HardBot ───────────────────────────────────────────────────────────────

TEST_CASE("HardBot: exact Bayesian update is deterministic (no noise)", "[bots][hard]") {
    std::array<int,4> hand = {2, 3, 2, 3};
    auto bot1 = make_hard_bot(cfg_default(), kSeed);
    auto bot2 = make_hard_bot(cfg_default(), kSeed);
    bot1->on_event(make_round_start(hand));
    bot2->on_event(make_round_start(hand));

    BotTradeEvent trade{Suit::Clubs, 8, std::nullopt};
    bot1->on_event(trade);
    bot2->on_event(trade);

    const auto& w1 = bot1->deck_weights();
    const auto& w2 = bot2->deck_weights();
    for (int d = 0; d < 12; ++d)
        REQUIRE(w1[d] == Catch::Approx(w2[d]).margin(1e-6f));
}

TEST_CASE("HardBot: lock-in triggers when max deck weight exceeds 0.95", "[bots][hard]") {
    std::array<int,4> hand = {2, 3, 2, 3};
    auto bot = make_hard_bot(cfg_default(), kSeed);
    bot->on_event(make_round_start(hand));
    REQUIRE_FALSE(bot->is_locked_in());

    // d0 = {C:10,D:8,H:10,S:12}, goal=Clubs → lock to Clubs
    std::array<float, 12> w{};
    w[0] = 0.97f;
    for (int d = 1; d < 12; ++d) w[d] = 0.03f / 11.0f;
    bot->set_deck_weights(w);

    // Any trade calls check_lock_in()
    bot->on_event(BotTradeEvent{Suit::Clubs, 5, std::nullopt});

    REQUIRE(bot->is_locked_in());
    REQUIRE(bot->locked_goal() == 0);  // Clubs
}

TEST_CASE("HardBot: after lock-in decide() bids on locked goal suit", "[bots][hard]") {
    std::array<int,4> hand = {0, 3, 3, 4};
    auto bot = make_hard_bot(cfg_default(), kSeed);
    bot->on_event(make_round_start(hand));

    std::array<float, 12> w{};
    w[0] = 0.97f;
    for (int d = 1; d < 12; ++d) w[d] = 0.03f / 11.0f;
    bot->set_deck_weights(w);
    bot->on_event(BotTradeEvent{Suit::Clubs, 5, std::nullopt});
    REQUIRE(bot->is_locked_in());

    auto actions = bot->decide(make_snapshot(hand, 120.0f));

    bool bids_clubs = std::any_of(actions.begin(), actions.end(),
        [](const BotAction& a) {
            if (const auto* s = std::get_if<BotSubmitOrder>(&a))
                return s->suit == Suit::Clubs && s->side == Side::Buy;
            return false;
        });
    REQUIRE(bids_clubs);
}
