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
    c.quoting_kappa         = 1.0f;  // deterministic for tests
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

// ── T5: qty-aware strategy tests ──────────────────────────────────────────────

// Helper: does the action list contain a BUY submit for the given suit?
static bool contains_buy(const std::vector<BotAction>& acts, Suit s) {
    return std::any_of(acts.begin(), acts.end(), [s](const BotAction& a) {
        if (const auto* sub = std::get_if<BotSubmitOrder>(&a))
            return sub->suit == s && sub->side == Side::Buy;
        return false;
    });
}
static bool contains_cancel(const std::vector<BotAction>& acts) {
    return std::any_of(acts.begin(), acts.end(), [](const BotAction& a) {
        return std::holds_alternative<BotCancelOrder>(a);
    });
}

TEST_CASE("EasyBot: gap_fill skips bid when opposite ask is thick (qty>=3)", "[bots][easy][qty]") {
    // hand {0,0,0,7}: EasyBot perceives goal=Clubs. EV_clubs is high (~29 with bonus).
    // We use ask=100 so the ratio 100/EV >> taker_threshold → taker never fires.
    // gap_fill should add a passive bid when no thick ask is present, and suppress
    // it when best_ask_qty >= 3.
    std::array<int,4> hand = {0, 0, 0, 7};
    EasyBot bot(cfg_default(), kSeed);
    bot.on_event(make_round_start(hand));

    // No ask info: bot should place a gap-fill bid for Clubs
    auto snap_normal = make_snapshot(hand, 120.0f);
    auto acts_normal = bot.decide(snap_normal);
    REQUIRE(contains_buy(acts_normal, Suit::Clubs));

    // Thick ask on Clubs at price 100 (ratio>>1 → taker won't cross; gap_fill skips bid)
    auto snap_thick = make_snapshot(hand, 120.0f);
    snap_thick.best_ask[0]     = 100;
    snap_thick.best_ask_qty[0] = 5;
    auto acts_thick = bot.decide(snap_thick);
    REQUIRE_FALSE(contains_buy(acts_thick, Suit::Clubs));
}

TEST_CASE("MediumBot: taker_scan crosses large resting ask at relaxed threshold",
          "[bots][medium][qty]") {
    // With uniform hand {2,2,2,2} → EV_clubs ≈ 9.17 (all suits equal).
    // Ask at 9 → ratio ≈ 0.981 > taker_threshold (0.95) → no TAKER cross normally.
    // gap_fill may still emit a passive bid at a different price (≈7-8), not at 9.
    // With best_ask_qty[Clubs] = 3 → eff_threshold = 1.00 → 0.981 ≤ 1.00 → taker cross at 9.
    std::array<int,4> hand = {2, 2, 2, 2};
    auto bot = make_medium_bot(cfg_default(), kSeed);
    bot->on_event(make_round_start(hand));

    // Helper: is there a buy submit AT exactly the ask price?
    auto taker_crosses_at = [](const std::vector<BotAction>& acts, Suit s, int32_t px) {
        return std::any_of(acts.begin(), acts.end(), [s, px](const BotAction& a) {
            if (const auto* sub = std::get_if<BotSubmitOrder>(&a))
                return sub->suit == s && sub->side == Side::Buy && sub->price == px;
            return false;
        });
    };

    // Without large qty: no taker cross at ask price 9 (gap_fill may bid lower)
    auto snap_no_qty = make_snapshot(hand, 120.0f);
    snap_no_qty.best_ask[0] = 9;
    auto acts_no = bot->decide(snap_no_qty);
    REQUIRE_FALSE(taker_crosses_at(acts_no, Suit::Clubs, 9));

    // With large qty: taker crosses at ask price 9
    auto snap_large = make_snapshot(hand, 120.0f);
    snap_large.best_ask[0]     = 9;
    snap_large.best_ask_qty[0] = 3;
    auto acts_large = bot->decide(snap_large);
    REQUIRE(taker_crosses_at(acts_large, Suit::Clubs, 9));
}

TEST_CASE("MediumBot: gap_fill bids lower price on thin book (qty<=1)",
          "[bots][medium][qty]") {
    // With uniform hand {2,2,2,2} → EV_clubs ≈ 9.17.
    // Normal bid: floor(9.17 * 0.90) = 8.
    // Thin bid:   floor(9.17 * 0.85) = 7.
    std::array<int,4> hand = {2, 2, 2, 2};
    auto bot_normal = make_medium_bot(cfg_default(), kSeed);
    auto bot_thin   = make_medium_bot(cfg_default(), kSeed);
    bot_normal->on_event(make_round_start(hand));
    bot_thin->on_event(make_round_start(hand));

    // Normal: no ask qty info (nullopt treated as thin — both should widen here)
    // Use qty=5 to mark normal liquidity
    auto snap_normal = make_snapshot(hand, 120.0f);
    snap_normal.best_ask_qty[0] = 5;  // thick → normal discount
    auto acts_normal = bot_normal->decide(snap_normal);

    // Thin: qty=1
    auto snap_thin = make_snapshot(hand, 120.0f);
    snap_thin.best_ask_qty[0] = 1;    // thin → wider margin
    auto acts_thin = bot_thin->decide(snap_thin);

    // Find bid prices for Clubs in each set
    int32_t price_normal = -1, price_thin = -1;
    for (const auto& a : acts_normal)
        if (const auto* s = std::get_if<BotSubmitOrder>(&a))
            if (s->suit == Suit::Clubs && s->side == Side::Buy) price_normal = s->price;
    for (const auto& a : acts_thin)
        if (const auto* s = std::get_if<BotSubmitOrder>(&a))
            if (s->suit == Suit::Clubs && s->side == Side::Buy) price_thin = s->price;

    REQUIRE(price_normal > 0);
    REQUIRE(price_thin > 0);
    REQUIRE(price_thin < price_normal);  // wider margin → lower bid
}

TEST_CASE("HardBot: taker_scan skips flash ask (qty==1)", "[bots][hard][qty]") {
    // EV_clubs is high (Spades-heavy hand → goal=Clubs) so bot would normally cross.
    // With qty=1 (flash), taker skips the ask. gap_fill may still add a passive bid
    // at a different price (capped at 20), so we check for a cross AT the ask price.
    std::array<int,4> hand = {0, 0, 0, 7};
    auto bot = make_hard_bot(cfg_default(), kSeed);
    bot->on_event(make_round_start(hand));

    // Helper: buy submit at exactly ask price?
    auto cross_at = [](const std::vector<BotAction>& acts, Suit s, int32_t px) {
        return std::any_of(acts.begin(), acts.end(), [s, px](const BotAction& a) {
            if (const auto* sub = std::get_if<BotSubmitOrder>(&a))
                return sub->suit == s && sub->side == Side::Buy && sub->price == px;
            return false;
        });
    };

    // No qty: taker crosses at ask price 5
    auto snap_no_qty = make_snapshot(hand, 120.0f);
    snap_no_qty.best_ask[0] = 5;
    auto acts_no = bot->decide(snap_no_qty);
    REQUIRE(cross_at(acts_no, Suit::Clubs, 5));

    // Flash qty=1: taker skips; no cross at ask price 5 (passive bid at 20 instead)
    auto snap_flash = make_snapshot(hand, 120.0f);
    snap_flash.best_ask[0]     = 5;
    snap_flash.best_ask_qty[0] = 1;
    auto acts_flash = bot->decide(snap_flash);
    REQUIRE_FALSE(cross_at(acts_flash, Suit::Clubs, 5));
}

TEST_CASE("HardBot: review_pending cancels bid jumped in queue", "[bots][hard][qty]") {
    // Bot has a resting bid at price 7 for Clubs. Market best_bid rises to 8.
    // HardBot should cancel the stale bid.
    std::array<int,4> hand = {1, 2, 2, 5};
    auto bot = make_hard_bot(cfg_default(), kSeed);
    bot->on_event(make_round_start(hand));

    // Inject a pending order as if bot had acked a bid at 7
    bot->on_event(BotOrderAckEvent{"42", Suit::Clubs, Side::Buy, 7});

    // Market best_bid is now 8 — someone jumped our bid
    auto snap = make_snapshot(hand, 120.0f);
    snap.best_bid[0] = 8;

    auto actions = bot->decide(snap);
    REQUIRE(contains_cancel(actions));
}

TEST_CASE("HardBot: locked taker crosses goal-suit ask more aggressively with large qty",
          "[bots][hard][qty]") {
    // Goal: show that locking on a suit AND having large qty gives a wider threshold
    // (+0.10) than just large qty alone (+0.05), enabling a cross that neither alone allows.
    //
    // With player_count=1000 the majority-bonus per-player share ≈ 0 so EV ≈ P*10.
    // Weights: w[0]=0.97 (deck 0, goal=Clubs) → EV_clubs ≈ 9.75 + ~0.10 = 9.85.
    // Ask=10: ratio = 10/9.85 ≈ 1.015.
    //   no qty:         eff_threshold=0.95   → 1.015 > 0.95 → no cross
    //   large + locked: eff_threshold=1.05   → 1.015 ≤ 1.05 → cross
    std::array<int,4> hand = {2, 2, 2, 2};
    auto bot = make_hard_bot(cfg_default(), kSeed);
    // player_count=1000 suppresses majority bonus so EV stays near P*10 (~9.8)
    bot->on_event(make_round_start(hand, 240.0f, 0, 1000));

    std::array<float, 12> w{};
    w[0] = 0.97f;
    for (int d = 1; d < 12; ++d) w[d] = 0.03f / 11.0f;
    bot->set_deck_weights(w);
    // Diamonds@5 → all decks L=0.5 → weights unchanged → max_w=0.97>0.95 → lock-in
    bot->on_event(BotTradeEvent{Suit::Diamonds, 5, std::nullopt});
    REQUIRE(bot->is_locked_in());
    REQUIRE(bot->locked_goal() == 0);  // Clubs

    auto cross_at = [](const std::vector<BotAction>& acts, Suit s, int32_t px) {
        return std::any_of(acts.begin(), acts.end(), [s, px](const BotAction& a) {
            if (const auto* sub = std::get_if<BotSubmitOrder>(&a))
                return sub->suit == s && sub->side == Side::Buy && sub->price == px;
            return false;
        });
    };

    // No large qty: ratio≈1.015 > threshold 0.95 → no taker cross at ask=10
    auto snap_no = make_snapshot(hand, 120.0f);
    snap_no.best_ask[0] = 10;
    auto acts_no = bot->decide(snap_no);
    REQUIRE_FALSE(cross_at(acts_no, Suit::Clubs, 10));

    // Large qty + locked goal: ratio≈1.015 ≤ eff_threshold 1.05 → cross at 10
    auto snap_locked_large = make_snapshot(hand, 120.0f);
    snap_locked_large.best_ask[0]     = 10;
    snap_locked_large.best_ask_qty[0] = 3;
    auto acts_locked = bot->decide(snap_locked_large);
    REQUIRE(cross_at(acts_locked, Suit::Clubs, 10));
}

// ── T6: passive quoting incentive ────────────────────────────────────────────

// Helper: does the action list contain any BUY or SELL submit?
static bool has_any_submit(const std::vector<BotAction>& acts) {
    return std::any_of(acts.begin(), acts.end(), [](const BotAction& a) {
        return std::holds_alternative<BotSubmitOrder>(a);
    });
}

TEST_CASE("EasyBot: passive_quote fires when kappa=1 and primary loop produces nothing",
          "[bots][easy][quoting]") {
    // Use hand with no cards (can't taker, can't gap_fill sell) and hand_size_cap=5 >0
    // so bid can still be placed. With kappa=1.0 (set in cfg_default) and an otherwise
    // empty tick, passive_quote must emit at least one submit.
    std::array<int,4> hand = {0, 0, 0, 0};
    EasyBot bot(cfg_default(), kSeed);
    bot.on_event(make_round_start(hand));

    // No book info, no pending, no cards to sell → taker empty, review empty, gap_fill empty
    // (gap_fill skips asks because hand[s]==0; bids may fire too — but kappa=1 ensures quote)
    auto snap = make_snapshot(hand, 120.0f);
    auto acts = bot.decide(snap);
    // With kappa=1 the passive_quote must add a bid for the highest-EV suit
    REQUIRE(has_any_submit(acts));
}

TEST_CASE("EasyBot: passive_quote suppressed when kappa=0", "[bots][easy][quoting]") {
    auto cfg = cfg_default();
    cfg.quoting_kappa = 0.0f;
    std::array<int,4> hand = {0, 0, 0, 0};
    EasyBot bot(cfg, kSeed);
    bot.on_event(make_round_start(hand));

    auto snap = make_snapshot(hand, 120.0f, 0, 4, 0);  // balance=0 → gap_fill can't bid either
    auto acts = bot.decide(snap);
    REQUIRE_FALSE(has_any_submit(acts));
}

TEST_CASE("MediumBot: passive_quote fires when kappa=1 and primary loop produces nothing",
          "[bots][medium][quoting]") {
    std::array<int,4> hand = {0, 0, 0, 0};
    auto bot = make_medium_bot(cfg_default(), kSeed);
    bot->on_event(make_round_start(hand));

    auto snap = make_snapshot(hand, 120.0f);
    auto acts = bot->decide(snap);
    REQUIRE(has_any_submit(acts));
}

TEST_CASE("HardBot: passive_quote fires when kappa=1 and primary loop produces nothing",
          "[bots][hard][quoting]") {
    std::array<int,4> hand = {0, 0, 0, 0};
    auto bot = make_hard_bot(cfg_default(), kSeed);
    bot->on_event(make_round_start(hand));

    auto snap = make_snapshot(hand, 120.0f);
    auto acts = bot->decide(snap);
    REQUIRE(has_any_submit(acts));
}

TEST_CASE("HardBot: passive_quote suppressed during endgame lock-in", "[bots][hard][quoting]") {
    auto cfg = cfg_default();
    cfg.endgame_threshold_s = 30;
    cfg.quoting_kappa       = 1.0f;

    std::array<int,4> hand = {0, 0, 0, 0};
    auto bot = make_hard_bot(cfg, kSeed);
    bot->on_event(make_round_start(hand));

    // Force lock-in
    std::array<float, 12> w{};
    w[0] = 0.97f;
    for (int d = 1; d < 12; ++d) w[d] = 0.03f / 11.0f;
    bot->set_deck_weights(w);
    bot->on_event(BotTradeEvent{Suit::Diamonds, 5, std::nullopt});
    REQUIRE(bot->is_locked_in());

    // Within endgame window (time_remaining < threshold) with balance=0:
    // locked_actions can't bid either → only passive_quote might fire, but endgame suppresses it.
    auto snap_endgame = make_snapshot(hand, 10.0f, 0, 4, 0);  // 10s < 30s threshold, balance=0
    auto acts = bot->decide(snap_endgame);
    REQUIRE_FALSE(has_any_submit(acts));
}

TEST_CASE("HardBot: passive_quote suppressed when at max_concurrent_orders",
          "[bots][hard][quoting]") {
    auto cfg = cfg_default();
    cfg.quoting_kappa         = 1.0f;
    cfg.max_concurrent_orders = 2;

    std::array<int,4> hand = {0, 0, 0, 0};
    auto bot = make_hard_bot(cfg, kSeed);
    bot->on_event(make_round_start(hand));

    // Simulate 2 resting orders by injecting acks for two suits
    bot->on_event(BotOrderAckEvent{"o1", Suit::Clubs,    Side::Buy, 5});
    bot->on_event(BotOrderAckEvent{"o2", Suit::Diamonds, Side::Buy, 5});

    auto snap = make_snapshot(hand, 120.0f, 0, 4, 0);  // balance=0 prevents new bids from gap_fill
    auto acts = bot->decide(snap);
    // All capacity used → passive_quote suppressed; taker/review/fill all empty → no actions
    REQUIRE_FALSE(has_any_submit(acts));
}
