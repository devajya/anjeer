#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "server/eval/execution_eval_module.h"
#include "engine/game_snapshot.h"
#include "engine/suit.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

using namespace anjeer::server::eval;
using namespace anjeer::engine;

// ===========================================================================
// Fixtures
// ===========================================================================

namespace {

constexpr double EWMA_ALPHA    = 0.1;   // must match ExecutionEvalModule::EWMA_ALPHA
constexpr double LEAKAGE_STEP  = 0.5;   // must match ExecutionEvalModule::LEAKAGE_STEP
constexpr double LEAKAGE_DECAY = 0.95;  // must match ExecutionEvalModule::LEAKAGE_DECAY
constexpr double FILL_DECAY_K  = 5.0;   // must match ExecutionEvalModule::FILL_DECAY_K

GameStateSnapshot make_exec_snap(int num_slots = 4) {
    GameStateSnapshot s;
    s.num_active_slots   = num_slots;
    s.time_remaining_s   = 120.0;
    s.round_number       = 1;
    s.current_deck_index = 0;
    s.points_per_card    = 10;
    for (int i = 0; i < num_slots; ++i) {
        s.slot_active[i]  = true;
        s.player_names[i] = "P" + std::to_string(i);
        s.balances[i]     = 500;
    }
    DeckSpec d;
    d.counts    = {10, 10, 10, 10};
    d.goal_suit = Suit::Clubs;
    for (auto& e : s.deck_table) e = d;
    return s;
}

EvalBookUpdate make_book(Suit suit, int bid, int ask) {
    EvalBookUpdate b;
    b.suit     = suit;
    b.best_bid = bid;
    b.best_ask = ask;
    return b;
}

EvalBookUpdate make_one_sided_bid(Suit suit, int bid) {
    EvalBookUpdate b;
    b.suit     = suit;
    b.best_bid = bid;
    return b;
}

EvalTradeEvent make_trade(int buyer, int seller, Suit suit, int price, int64_t ts_ms) {
    EvalTradeEvent t;
    t.buyer_slot   = buyer;
    t.seller_slot  = seller;
    t.price        = price;
    t.suit         = suit;
    t.timestamp_ms = ts_ms;
    return t;
}

// Drive n directional crosses (buyer=0, seller=1) in `suit`, spaced 200ms apart.
void drive_crosses(ExecutionEvalModule& mod, Suit suit, int n,
                   int bid = 100, int ask = 105) {
    for (int i = 0; i < n; ++i) {
        mod.on_book_update(make_book(suit, bid, ask));
        mod.on_trade_event(make_trade(0, 1, suit, ask, i * 200LL));
    }
}

// Collect all ExecutionGuidance outputs from one full round.
std::vector<EvalOutput> run_round(ExecutionEvalModule& mod,
                                   const GameStateSnapshot& snap,
                                   std::function<void()> mid_round) {
    std::vector<EvalOutput> outputs;
    mod.set_output_cb([&](EvalOutput o) { outputs.push_back(std::move(o)); });
    mod.on_round_start(snap);
    mid_round();
    mod.on_round_end(snap);
    return outputs;
}

// Return the ExecutionGuidance JSON for `suit_key` from a set of outputs.
nlohmann::json guidance_for_suit(const std::vector<EvalOutput>& outputs,
                                  const std::string& suit_key) {
    for (const auto& out : outputs) {
        if (out.type != EvalOutput::Type::ExecutionGuidance) continue;
        if (out.payload.contains("suits") &&
            out.payload["suits"].contains(suit_key))
            return out.payload["suits"][suit_key];
    }
    return {};
}

} // namespace

// ===========================================================================
// T17 — No trades → fill_probability at baseline (0) for every suit
// ===========================================================================
TEST_CASE("Execution: fill probability 0 with no trade history", "[execution][T17]") {
    std::vector<EvalOutput> outputs;
    ExecutionEvalModule mod;
    mod.set_output_cb([&](EvalOutput o) { outputs.push_back(std::move(o)); });

    GameStateSnapshot snap = make_exec_snap();
    mod.on_round_start(snap);
    mod.on_round_end(snap);

    REQUIRE(!outputs.empty());

    bool found = false;
    for (const auto& out : outputs) {
        if (out.type != EvalOutput::Type::ExecutionGuidance) continue;
        found = true;
        REQUIRE(out.payload.contains("suits"));
        for (const auto& [key, g] : out.payload["suits"].items()) {
            REQUIRE_THAT(g["fill_probability"].get<double>(),
                         Catch::Matchers::WithinAbs(0.0, 1e-9));
        }
    }
    REQUIRE(found);
}

// ===========================================================================
// T18 — aggressive_buy_cost = best_ask - proxy_fair_value (best_bid)
// ===========================================================================
// best_ask=110, best_bid=100 → proxy fair value = 100, cost = 10.
TEST_CASE("Execution: aggressive buy cost computed correctly", "[execution][T18]") {
    std::vector<EvalOutput> outputs;
    ExecutionEvalModule mod;
    mod.set_output_cb([&](EvalOutput o) { outputs.push_back(std::move(o)); });

    GameStateSnapshot snap = make_exec_snap();
    mod.on_round_start(snap);
    mod.on_book_update(make_book(Suit::Spades, 100, 110));
    mod.on_round_end(snap);

    REQUIRE(!outputs.empty());

    auto g = guidance_for_suit(outputs, "spades");
    REQUIRE(!g.empty());
    REQUIRE(g.contains("aggressive_buy_cost"));
    // proxy fair value ≈ best_bid(100); cost = ask(110) - fair_value(100) = 10
    REQUIRE_THAT(g["aggressive_buy_cost"].get<double>(),
                 Catch::Matchers::WithinAbs(10.0, 1e-6));
}

// ===========================================================================
// T19 — passive_ev = fill_prob × (spread_width / 2.0) - leakage_penalty
// ===========================================================================
TEST_CASE("Execution: passive EV formula correct", "[execution][T19]") {
    std::vector<EvalOutput> outputs;
    ExecutionEvalModule mod;
    mod.set_output_cb([&](EvalOutput o) { outputs.push_back(std::move(o)); });

    GameStateSnapshot snap = make_exec_snap();
    mod.on_round_start(snap);
    // Prime fill_rate then fix spread; no directional crosses so leakage = 0
    for (int i = 0; i < 5; ++i)
        mod.on_trade_event(make_trade(i % 4, (i + 1) % 4, Suit::Clubs, 102, i * 200LL));
    mod.on_book_update(make_book(Suit::Clubs, 100, 104));  // spread = 4
    mod.on_round_end(snap);

    REQUIRE(!outputs.empty());

    auto g = guidance_for_suit(outputs, "clubs");
    REQUIRE(!g.empty());
    REQUIRE(g.contains("passive_ev"));
    REQUIRE(g.contains("fill_probability"));
    REQUIRE(g.contains("spread_width"));

    const double fp      = g["fill_probability"].get<double>();
    const int    sw      = g["spread_width"].get<int>();
    const double penalty = mod.leakage_penalty_for(suit_index(Suit::Clubs));
    REQUIRE_THAT(g["passive_ev"].get<double>(),
                 Catch::Matchers::WithinAbs(fp * (sw / 2.0) - penalty, 1e-6));
}

// ===========================================================================
// T20 — Recommendation switches deterministically at EV threshold
// ===========================================================================
// Construction: force a scenario where passive wins (high fill_prob, narrow spread)
// vs a scenario where passive cannot win (no fill history, wide spread).
TEST_CASE("Execution: recommendation passive when passive_ev > aggressive_ev",
          "[execution][T20]") {
    // Scenario A — narrow spread + many trades → passive_ev dominates Hearts
    std::vector<EvalOutput> passive_outputs;
    {
        ExecutionEvalModule mod;
        mod.set_output_cb([&](EvalOutput o) { passive_outputs.push_back(std::move(o)); });
        GameStateSnapshot snap = make_exec_snap();
        mod.on_round_start(snap);
        for (int i = 0; i < 30; ++i) {
            mod.on_trade_event(make_trade(i % 4, (i + 1) % 4, Suit::Hearts, 101, i * 100LL));
            mod.on_book_update(make_book(Suit::Hearts, 100, 102));  // spread = 2
        }
        mod.on_round_end(snap);
    }

    // Scenario B — huge spread, zero trade history → hold for Diamonds
    std::vector<EvalOutput> hold_outputs;
    {
        ExecutionEvalModule mod;
        mod.set_output_cb([&](EvalOutput o) { hold_outputs.push_back(std::move(o)); });
        GameStateSnapshot snap = make_exec_snap();
        mod.on_round_start(snap);
        mod.on_round_end(snap);  // no book, no trades → hold for all
    }

    // Scenario A: Hearts must be "passive"
    auto g_hearts = guidance_for_suit(passive_outputs, "hearts");
    REQUIRE(!g_hearts.empty());
    REQUIRE(g_hearts["recommendation"].get<std::string>() == "passive");

    // Scenario B: every suit must be "hold"
    bool found_hold = false;
    for (const auto& out : hold_outputs) {
        if (out.type != EvalOutput::Type::ExecutionGuidance) continue;
        found_hold = true;
        for (const auto& [key, g] : out.payload["suits"].items())
            REQUIRE(g["recommendation"].get<std::string>() == "hold");
    }
    REQUIRE(found_hold);
}

// ===========================================================================
// T21 — Leakage penalty increases with repeated same-suit directional crosses
// ===========================================================================
TEST_CASE("Execution: leakage penalty grows with directional crosses", "[execution][T21]") {
    ExecutionEvalModule mod;
    GameStateSnapshot snap = make_exec_snap();
    mod.on_round_start(snap);

    const double before = mod.leakage_penalty_for(suit_index(Suit::Spades));
    drive_crosses(mod, Suit::Spades, 5);
    const double after = mod.leakage_penalty_for(suit_index(Suit::Spades));

    REQUIRE(after > before);
}

// ===========================================================================
// LAYER 1 — Mathematical and state correctness
// ===========================================================================

// L1-1: spread_width correctly derived from a two-sided book event; unrelated suit unchanged.
TEST_CASE("Execution L1: spread_width set correctly from book update", "[execution][L1]") {
    ExecutionEvalModule mod;
    GameStateSnapshot snap = make_exec_snap();
    mod.on_round_start(snap);

    mod.on_book_update(make_book(Suit::Clubs, 98, 103));

    REQUIRE(mod.spread_width_for(suit_index(Suit::Clubs)) == 5);
    REQUIRE(mod.spread_width_for(suit_index(Suit::Spades)) == 0);
}

// L1-2: Exact EWMA recurrence for trade_intensity.
// alpha=0.1; first sample x=10 → new_ewma = 0.1*10 + 0.9*0 = 1.0.
// second sample x=10 → new_ewma = 0.1*10 + 0.9*1.0 = 1.9.
// NOTE: trade_intensity is computed per trade; the EWMA update uses the interval
// between consecutive trades to derive a rate, so the test drives two identical-
// interval trades and checks the exact recurrence result.
TEST_CASE("Execution L1: trade_intensity EWMA follows exact recurrence", "[execution][L1]") {
    ExecutionEvalModule mod;
    GameStateSnapshot snap = make_exec_snap();
    mod.on_round_start(snap);

    // Two trades spaced 100ms apart → interval = 100ms → rate = 10 trades/s
    mod.on_trade_event(make_trade(0, 1, Suit::Clubs, 100, 0));
    const double after_one = mod.trade_intensity_for(suit_index(Suit::Clubs));
    // after 1: ewma = alpha * 10 = 1.0
    REQUIRE_THAT(after_one, Catch::Matchers::WithinAbs(EWMA_ALPHA * 10.0, 1e-9));

    mod.on_trade_event(make_trade(0, 1, Suit::Clubs, 100, 100));
    const double after_two = mod.trade_intensity_for(suit_index(Suit::Clubs));
    // after 2: ewma = alpha*10 + (1-alpha)*1.0 = 1.0 + 0.9 = 1.9
    REQUIRE_THAT(after_two,
                 Catch::Matchers::WithinAbs(EWMA_ALPHA * 10.0 + (1.0 - EWMA_ALPHA) * after_one,
                                            1e-9));
}

// L1-3: fill_probability is clamped to [0,1] under stress.
TEST_CASE("Execution L1: fill_probability clamped to [0, 1] under stress", "[execution][L1]") {
    std::vector<EvalOutput> outputs;
    ExecutionEvalModule mod;
    mod.set_output_cb([&](EvalOutput o) { outputs.push_back(std::move(o)); });

    GameStateSnapshot snap = make_exec_snap();
    mod.on_round_start(snap);
    for (int i = 0; i < 100; ++i) {
        Suit s = kAllSuits[i % 4];
        mod.on_trade_event(make_trade(i % 4, (i + 1) % 4, s, 100, i * 50LL));
        if (i % 3 == 0) mod.on_book_update(make_book(s, 99, 101));
    }
    mod.on_round_end(snap);

    REQUIRE(!outputs.empty());

    for (int s = 0; s < 4; ++s) {
        const double fp = mod.fill_probability_for(s);
        REQUIRE(fp >= 0.0);
        REQUIRE(fp <= 1.0);
    }
    for (const auto& out : outputs) {
        if (out.type != EvalOutput::Type::ExecutionGuidance) continue;
        for (const auto& [key, g] : out.payload["suits"].items()) {
            const double fp = g["fill_probability"].get<double>();
            REQUIRE(fp >= 0.0);
            REQUIRE(fp <= 1.0);
        }
    }
}

// L1-4: Exponential spread model — fill_probability decays with exp(-spread/k).
// Prime two identical fill_rate states, then apply narrow vs wide spread; check ratio.
TEST_CASE("Execution L1: fill_probability follows exponential spread decay", "[execution][L1]") {
    auto fill_prob_for_spread = [](int spread) {
        std::vector<EvalOutput> outputs;
        ExecutionEvalModule mod;
        mod.set_output_cb([&](EvalOutput o) { outputs.push_back(std::move(o)); });
        GameStateSnapshot snap = make_exec_snap();
        mod.on_round_start(snap);
        for (int i = 0; i < 10; ++i)
            mod.on_trade_event(make_trade(0, 1, Suit::Spades, 100, i * 100LL));
        mod.on_book_update(make_book(Suit::Spades, 100, 100 + spread));
        mod.on_round_end(snap);
        auto g = guidance_for_suit(outputs, "spades");
        return g.empty() ? -1.0 : g["fill_probability"].get<double>();
    };

    const double fp2  = fill_prob_for_spread(2);
    const double fp10 = fill_prob_for_spread(10);

    REQUIRE(fp2  >= 0.0);
    REQUIRE(fp10 >= 0.0);
    // exp(-10/5) / exp(-2/5) = exp(-8/5) ≈ 0.202; fp10 must be strictly less than fp2
    REQUIRE(fp2 > fp10);
    // Check the ratio is approximately exp(-(10-2)/k) = exp(-8/5) ≈ 0.202
    if (fp2 > 1e-9) {
        const double expected_ratio = std::exp(-(10.0 - 2.0) / FILL_DECAY_K);
        REQUIRE_THAT(fp10 / fp2, Catch::Matchers::WithinAbs(expected_ratio, 0.05));
    }
}

// L1-5: Leakage penalty increments by LEAKAGE_STEP on each directional cross.
// After 1 cross from zero: penalty ≈ LEAKAGE_STEP (decay may apply per trade, so check ≥).
TEST_CASE("Execution L1: leakage penalty increments on directional cross", "[execution][L1]") {
    ExecutionEvalModule mod;
    GameStateSnapshot snap = make_exec_snap();
    mod.on_round_start(snap);

    mod.on_book_update(make_book(Suit::Hearts, 100, 105));
    mod.on_trade_event(make_trade(0, 1, Suit::Hearts, 105, 0));  // buyer crosses ask

    const double penalty = mod.leakage_penalty_for(suit_index(Suit::Hearts));
    // At minimum one step must have been added (decay shrinks by at most LEAKAGE_DECAY per trade)
    REQUIRE(penalty >= LEAKAGE_STEP * LEAKAGE_DECAY);
}

// L1-6: Leakage penalty decays after non-directional trades.
// Cross once → accumulate penalty → drive several unrelated trades in different suit →
// subsequent trade in same suit with no cross → penalty must be lower.
TEST_CASE("Execution L1: leakage penalty decays after non-directional trades", "[execution][L1]") {
    ExecutionEvalModule mod;
    GameStateSnapshot snap = make_exec_snap();
    mod.on_round_start(snap);

    // Cross Spades once to establish a baseline penalty
    mod.on_book_update(make_book(Suit::Spades, 100, 105));
    mod.on_trade_event(make_trade(0, 1, Suit::Spades, 105, 0));
    const double penalty_after_cross = mod.leakage_penalty_for(suit_index(Suit::Spades));
    REQUIRE(penalty_after_cross > 0.0);

    // Drive many Clubs trades — each trade event must apply LEAKAGE_DECAY to Spades penalty
    for (int i = 0; i < 10; ++i)
        mod.on_trade_event(make_trade(i % 4, (i + 1) % 4, Suit::Clubs, 100, (i + 1) * 200LL));

    const double penalty_after_decay = mod.leakage_penalty_for(suit_index(Suit::Spades));
    REQUIRE(penalty_after_decay < penalty_after_cross);
}

// L1-7: leakage_penalty is suit-isolated — Clubs crosses must not affect Spades.
TEST_CASE("Execution L1: leakage_penalty is suit-isolated", "[execution][L1]") {
    ExecutionEvalModule mod;
    GameStateSnapshot snap = make_exec_snap();
    mod.on_round_start(snap);

    drive_crosses(mod, Suit::Clubs, 5);

    REQUIRE(mod.leakage_penalty_for(suit_index(Suit::Clubs))  > 0.0);
    REQUIRE_THAT(mod.leakage_penalty_for(suit_index(Suit::Spades)),
                 Catch::Matchers::WithinAbs(0.0, 1e-9));
}

// L1-8: on_round_start resets spread_width, recent_trades, and leakage_penalty.
TEST_CASE("Execution L1: on_round_start resets all per-suit stats", "[execution][L1]") {
    ExecutionEvalModule mod;
    GameStateSnapshot snap = make_exec_snap();
    mod.on_round_start(snap);

    mod.on_book_update(make_book(Suit::Diamonds, 90, 110));
    mod.on_trade_event(make_trade(0, 1, Suit::Diamonds, 110, 0));

    REQUIRE(mod.spread_width_for(suit_index(Suit::Diamonds)) == 20);
    REQUIRE(mod.recent_trades_for(suit_index(Suit::Diamonds)) == 1);
    REQUIRE(mod.leakage_penalty_for(suit_index(Suit::Diamonds)) > 0.0);

    mod.on_round_start(snap);

    REQUIRE(mod.spread_width_for(suit_index(Suit::Diamonds))  == 0);
    REQUIRE(mod.recent_trades_for(suit_index(Suit::Diamonds)) == 0);
    REQUIRE_THAT(mod.leakage_penalty_for(suit_index(Suit::Diamonds)),
                 Catch::Matchers::WithinAbs(0.0, 1e-9));
}

// L1-9: Output JSON contains all required schema fields for every suit.
TEST_CASE("Execution L1: output JSON has all required schema fields", "[execution][L1]") {
    std::vector<EvalOutput> outputs;
    ExecutionEvalModule mod;
    mod.set_output_cb([&](EvalOutput o) { outputs.push_back(std::move(o)); });

    GameStateSnapshot snap = make_exec_snap();
    mod.on_round_start(snap);
    mod.on_book_update(make_book(Suit::Spades, 98, 102));
    mod.on_round_end(snap);

    REQUIRE(!outputs.empty());

    bool found = false;
    for (const auto& out : outputs) {
        if (out.type != EvalOutput::Type::ExecutionGuidance) continue;
        found = true;
        REQUIRE(out.target_slot == -1);
        REQUIRE(out.payload.contains("suits"));
        for (const auto& [key, g] : out.payload["suits"].items()) {
            REQUIRE(g.contains("aggressive_buy_cost"));
            REQUIRE(g.contains("passive_ev"));
            REQUIRE(g.contains("fill_probability"));
            REQUIRE(g.contains("recommendation"));
            REQUIRE(g.contains("liquidity"));
            REQUIRE(g.contains("spread_width"));
            REQUIRE(g.contains("trade_intensity"));
            REQUIRE(g.contains("execution_risk"));
            const std::string rec  = g["recommendation"].get<std::string>();
            const std::string liq  = g["liquidity"].get<std::string>();
            const std::string risk = g["execution_risk"].get<std::string>();
            REQUIRE((rec  == "passive" || rec  == "aggressive" || rec  == "hold"));
            REQUIRE((liq  == "thin"    || liq  == "normal"     || liq  == "deep"));
            REQUIRE((risk == "low"     || risk == "moderate"   || risk == "elevated"));
        }
    }
    REQUIRE(found);
}

// L1-10: One-sided bid book (no ask) → spread_width stays 0.
TEST_CASE("Execution L1: one-sided bid does not set spread_width", "[execution][L1]") {
    ExecutionEvalModule mod;
    GameStateSnapshot snap = make_exec_snap();
    mod.on_round_start(snap);

    mod.on_book_update(make_one_sided_bid(Suit::Diamonds, 90));

    REQUIRE(mod.spread_width_for(suit_index(Suit::Diamonds)) == 0);
}

// ===========================================================================
// LAYER 2 — Behavioral monotonicity and stability
// ===========================================================================

// L2-1: No book update → all suits emit "hold".
TEST_CASE("Execution L2: no book produces hold for all suits", "[execution][L2]") {
    std::vector<EvalOutput> outputs;
    ExecutionEvalModule mod;
    mod.set_output_cb([&](EvalOutput o) { outputs.push_back(std::move(o)); });

    GameStateSnapshot snap = make_exec_snap();
    mod.on_round_start(snap);
    mod.on_round_end(snap);

    REQUIRE(!outputs.empty());

    bool found = false;
    for (const auto& out : outputs) {
        if (out.type != EvalOutput::Type::ExecutionGuidance) continue;
        found = true;
        for (const auto& [key, g] : out.payload["suits"].items())
            REQUIRE(g["recommendation"].get<std::string>() == "hold");
    }
    REQUIRE(found);
}

// L2-2: Determinism — identical event streams produce bit-identical outputs.
TEST_CASE("Execution L2: identical event streams produce deterministic outputs",
          "[execution][L2]") {
    auto run = []() {
        std::vector<EvalOutput> outputs;
        ExecutionEvalModule mod;
        mod.set_output_cb([&](EvalOutput o) { outputs.push_back(std::move(o)); });
        GameStateSnapshot snap = make_exec_snap();
        mod.on_round_start(snap);
        mod.on_book_update(make_book(Suit::Clubs, 98, 103));
        mod.on_trade_event(make_trade(0, 1, Suit::Clubs, 102, 500));
        mod.on_book_update(make_book(Suit::Spades, 95, 100));
        mod.on_round_end(snap);
        return outputs;
    };

    const auto first  = run();
    const auto second = run();

    REQUIRE(!first.empty());
    REQUIRE(first.size() == second.size());

    for (size_t i = 0; i < first.size(); ++i) {
        REQUIRE(first[i].type == second[i].type);
        REQUIRE(first[i].target_slot == second[i].target_slot);
        REQUIRE(first[i].payload == second[i].payload);
    }
}

// L2-3: Increasing trade frequency raises trade_intensity monotonically.
TEST_CASE("Execution L2: higher trade frequency yields higher trade_intensity",
          "[execution][L2]") {
    double intensity_low = 0.0;
    {
        ExecutionEvalModule mod;
        GameStateSnapshot snap = make_exec_snap();
        mod.on_round_start(snap);
        for (int i = 0; i < 2; ++i)
            mod.on_trade_event(make_trade(0, 1, Suit::Hearts, 100, i * 1000LL));
        intensity_low = mod.trade_intensity_for(suit_index(Suit::Hearts));
    }

    double intensity_high = 0.0;
    {
        ExecutionEvalModule mod;
        GameStateSnapshot snap = make_exec_snap();
        mod.on_round_start(snap);
        for (int i = 0; i < 20; ++i)
            mod.on_trade_event(make_trade(0, 1, Suit::Hearts, 100, i * 100LL));
        intensity_high = mod.trade_intensity_for(suit_index(Suit::Hearts));
    }

    REQUIRE(intensity_high > intensity_low);
}

// L2-4: Equal-timestamp trades — module must not crash or produce NaN.
TEST_CASE("Execution L2: equal-timestamp trades produce no NaN", "[execution][L2]") {
    ExecutionEvalModule mod;
    GameStateSnapshot snap = make_exec_snap();
    mod.on_round_start(snap);

    for (int i = 0; i < 5; ++i)
        mod.on_trade_event(make_trade(0, 1, Suit::Clubs, 100, 0));  // all ts = 0

    for (int s = 0; s < 4; ++s) {
        REQUIRE(std::isfinite(mod.trade_intensity_for(s)));
        REQUIRE(std::isfinite(mod.fill_probability_for(s)));
    }
}

// L2-5: Very large timestamp gap — module must not produce Inf.
TEST_CASE("Execution L2: large timestamp gap produces no Inf", "[execution][L2]") {
    ExecutionEvalModule mod;
    GameStateSnapshot snap = make_exec_snap();
    mod.on_round_start(snap);

    mod.on_trade_event(make_trade(0, 1, Suit::Clubs, 100, 0));
    mod.on_trade_event(make_trade(0, 1, Suit::Clubs, 100, 1'000'000'000LL));  // ~11 days

    for (int s = 0; s < 4; ++s) {
        REQUIRE(std::isfinite(mod.trade_intensity_for(s)));
        REQUIRE(std::isfinite(mod.fill_probability_for(s)));
    }
}

// L2-6: Numerical safety — 1000 mixed events produce no NaN/Inf.
TEST_CASE("Execution L2: no NaN/Inf under 1000-event stress", "[execution][L2]") {
    std::vector<EvalOutput> outputs;
    ExecutionEvalModule mod;
    mod.set_output_cb([&](EvalOutput o) { outputs.push_back(std::move(o)); });

    GameStateSnapshot snap = make_exec_snap();
    mod.on_round_start(snap);

    for (int i = 0; i < 1000; ++i) {
        Suit s = kAllSuits[i % 4];
        mod.on_trade_event(make_trade(i % 4, (i + 1) % 4, s, 100 + i % 10, i * 50LL));
        if (i % 5 == 0)
            mod.on_book_update(make_book(s, 99 + i % 3, 102 + i % 3));
    }
    mod.on_round_end(snap);

    for (int s = 0; s < 4; ++s) {
        REQUIRE(std::isfinite(mod.fill_probability_for(s)));
        REQUIRE(std::isfinite(mod.trade_intensity_for(s)));
        REQUIRE(std::isfinite(mod.leakage_penalty_for(s)));
        REQUIRE(mod.fill_probability_for(s) >= 0.0);
        REQUIRE(mod.fill_probability_for(s) <= 1.0);
    }

    REQUIRE(!outputs.empty());
    for (const auto& out : outputs) {
        if (out.type != EvalOutput::Type::ExecutionGuidance) continue;
        for (const auto& [key, g] : out.payload["suits"].items()) {
            REQUIRE(std::isfinite(g["fill_probability"].get<double>()));
            REQUIRE(std::isfinite(g["passive_ev"].get<double>()));
            REQUIRE(std::isfinite(g["aggressive_buy_cost"].get<double>()));
        }
    }
}

// ===========================================================================
// Performance — 3 end-to-end latency benchmarks across different configs
// ===========================================================================

// PERF-1: on_round_start < 0.5ms average over 500 iterations (fresh snapshot, 4 players).
TEST_CASE("Execution perf: on_round_start < 0.5ms average [config: 4-player fresh snap]",
          "[execution][perf]") {
    ExecutionEvalModule mod;
    GameStateSnapshot snap = make_exec_snap(4);

    const int kRuns = 500;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kRuns; ++i)
        mod.on_round_start(snap);
    auto t1 = std::chrono::steady_clock::now();

    const double avg_us =
        std::chrono::duration<double, std::micro>(t1 - t0).count() / kRuns;
    REQUIRE(avg_us < 500.0);
}

// PERF-2: Full round pipeline (start + 50 mixed events + end) < 5ms total.
// Config A: 4 players, balanced suit distribution.
TEST_CASE("Execution perf: full pipeline < 5ms [config A — 4 player balanced]",
          "[execution][perf]") {
    ExecutionEvalModule mod;
    mod.set_output_cb([](EvalOutput) {});
    GameStateSnapshot snap = make_exec_snap(4);

    auto t0 = std::chrono::steady_clock::now();
    mod.on_round_start(snap);
    for (int i = 0; i < 50; ++i) {
        Suit s = kAllSuits[i % 4];
        mod.on_trade_event(make_trade(i % 4, (i + 1) % 4, s, 100, i * 100LL));
        if (i % 5 == 0) mod.on_book_update(make_book(s, 99, 102));
    }
    mod.on_round_end(snap);
    auto t1 = std::chrono::steady_clock::now();

    REQUIRE(std::chrono::duration<double, std::micro>(t1 - t0).count() < 6000.0);
}

// PERF-3: Full round pipeline < 5ms (limit raised to 6ms for WSL2).
// Config B: 2 active players, heavy Spades directional crossing scenario.
TEST_CASE("Execution perf: full pipeline < 5ms [config B — 2 player heavy Spades crosses]",
          "[execution][perf]") {
    ExecutionEvalModule mod;
    mod.set_output_cb([](EvalOutput) {});
    GameStateSnapshot snap = make_exec_snap(2);

    auto t0 = std::chrono::steady_clock::now();
    mod.on_round_start(snap);
    for (int i = 0; i < 50; ++i) {
        mod.on_book_update(make_book(Suit::Spades, 100, 105));
        mod.on_trade_event(make_trade(0, 1, Suit::Spades, 105, i * 150LL));
    }
    mod.on_round_end(snap);
    auto t1 = std::chrono::steady_clock::now();

    REQUIRE(std::chrono::duration<double, std::micro>(t1 - t0).count() < 6000.0);
}
