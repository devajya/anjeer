#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "server/eval/accumulation_eval_module.h"
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

GameStateSnapshot make_acc_snap(int num_slots = 4) {
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

EvalTradeEvent make_trade(int buyer, int seller, Suit suit, int64_t ts_ms) {
    EvalTradeEvent t;
    t.buyer_slot   = buyer;
    t.seller_slot  = seller;
    t.price        = 100;
    t.suit         = suit;
    t.timestamp_ms = ts_ms;
    return t;
}

// Drive n one-sided buys of `suit` for slot 0 (slot 1 sells), starting at ts 0.
void drive_buys(AccumulationEvalModule& mod, Suit suit, int n) {
    for (int i = 0; i < n; ++i)
        mod.on_trade_event(make_trade(0, 1, suit, i * 200LL));
}

// Run a module from round_start through n buys and round_end;
// return the confidence for slot 0, or -1.0 if no output was emitted.
double confidence_after_n_buys(int n, Suit suit = Suit::Spades) {
    std::vector<EvalOutput> outputs;
    AccumulationEvalModule mod;
    mod.set_output_cb([&](EvalOutput o) { outputs.push_back(std::move(o)); });
    GameStateSnapshot snap = make_acc_snap();
    mod.on_round_start(snap);
    drive_buys(mod, suit, n);
    mod.on_round_end(snap);
    for (const auto& out : outputs) {
        if (out.type != EvalOutput::Type::AccumulationSignal) continue;
        for (const auto& p : out.payload["players"])
            if (p["slot"].get<int>() == 0)
                return p["confidence"].get<double>();
    }
    return -1.0;
}

} // namespace

// ===========================================================================
// T12 — Fresh round → all player signals are "Normal"
// ===========================================================================
TEST_CASE("Accumulation: all Normal at round start", "[accumulation][T12]") {
    std::vector<EvalOutput> outputs;
    AccumulationEvalModule mod;
    mod.set_output_cb([&](EvalOutput o) { outputs.push_back(std::move(o)); });

    GameStateSnapshot snap = make_acc_snap();
    mod.on_round_start(snap);
    mod.on_round_end(snap);

    REQUIRE(!outputs.empty());

    bool found_signal = false;
    for (const auto& out : outputs) {
        if (out.type != EvalOutput::Type::AccumulationSignal) continue;
        found_signal = true;
        for (const auto& player : out.payload["players"])
            REQUIRE(player["signal"].get<std::string>() == "Normal");
    }
    REQUIRE(found_signal);
}

// ===========================================================================
// T13 — Repeated one-sided buying raises signal level
// ===========================================================================
// Slot 0 buys Spades 8 consecutive times; signal must be Elevated or High.
TEST_CASE("Accumulation: one-sided buying raises signal level", "[accumulation][T13]") {
    std::vector<EvalOutput> outputs;
    AccumulationEvalModule mod;
    mod.set_output_cb([&](EvalOutput o) { outputs.push_back(std::move(o)); });

    GameStateSnapshot snap = make_acc_snap();
    mod.on_round_start(snap);
    drive_buys(mod, Suit::Spades, 8);
    mod.on_round_end(snap);

    REQUIRE(!outputs.empty());

    bool found_elevated = false;
    for (const auto& out : outputs) {
        if (out.type != EvalOutput::Type::AccumulationSignal) continue;
        for (const auto& player : out.payload["players"]) {
            if (player["slot"].get<int>() == 0) {
                const std::string sig = player["signal"].get<std::string>();
                if (sig == "Elevated" || sig == "High")
                    found_elevated = true;
            }
        }
    }
    REQUIRE(found_elevated);
}

// ===========================================================================
// T14 — EWMA baseline increases after sustained trade activity
// ===========================================================================
TEST_CASE("Accumulation: EWMA baseline increases after trade activity", "[accumulation][T14]") {
    AccumulationEvalModule mod;
    GameStateSnapshot snap = make_acc_snap();
    mod.on_round_start(snap);

    const double before = mod.ewma_baseline_for(suit_index(Suit::Spades));

    drive_buys(mod, Suit::Spades, 5);
    mod.on_round_end(snap);

    const double after = mod.ewma_baseline_for(suit_index(Suit::Spades));
    REQUIRE(after > before);
}

// ===========================================================================
// T15 — Confidence field is clamped to [0, 1]
// ===========================================================================
TEST_CASE("Accumulation: confidence in [0, 1]", "[accumulation][T15]") {
    std::vector<EvalOutput> outputs;
    AccumulationEvalModule mod;
    mod.set_output_cb([&](EvalOutput o) { outputs.push_back(std::move(o)); });

    GameStateSnapshot snap = make_acc_snap();
    mod.on_round_start(snap);
    for (int i = 0; i < 8; ++i)
        mod.on_trade_event(make_trade(i % 4, (i + 1) % 4, kAllSuits[i % 4], i * 200LL));
    mod.on_round_end(snap);

    REQUIRE(!outputs.empty());

    bool found_signal = false;
    for (const auto& out : outputs) {
        if (out.type != EvalOutput::Type::AccumulationSignal) continue;
        found_signal = true;
        for (const auto& player : out.payload["players"]) {
            const double conf = player["confidence"].get<double>();
            REQUIRE(conf >= 0.0);
            REQUIRE(conf <= 1.0);
        }
    }
    REQUIRE(found_signal);
}

// ===========================================================================
// T16 — Output payload matches the required JSON schema
// ===========================================================================
TEST_CASE("Accumulation: output JSON has required fields", "[accumulation][T16]") {
    std::vector<EvalOutput> outputs;
    AccumulationEvalModule mod;
    mod.set_output_cb([&](EvalOutput o) { outputs.push_back(std::move(o)); });

    GameStateSnapshot snap = make_acc_snap();
    mod.on_round_start(snap);
    mod.on_round_end(snap);

    REQUIRE(!outputs.empty());

    bool found_acc = false;
    for (const auto& out : outputs) {
        if (out.type != EvalOutput::Type::AccumulationSignal) continue;
        found_acc = true;
        REQUIRE(out.target_slot == -1);
        REQUIRE(out.payload.contains("players"));
        REQUIRE(out.payload["players"].is_array());
        for (const auto& player : out.payload["players"]) {
            REQUIRE(player.contains("slot"));
            REQUIRE(player.contains("player"));
            REQUIRE(player.contains("signal"));
            REQUIRE(player.contains("confidence"));
            REQUIRE(player.contains("primary_suit"));
            const std::string sig = player["signal"].get<std::string>();
            REQUIRE((sig == "Normal" || sig == "Elevated" || sig == "High"));
        }
    }
    REQUIRE(found_acc);
}

// ===========================================================================
// LAYER 1 — Mathematical and state correctness
// ===========================================================================

// L1-1: Conservation — buy increments buyer's delta; sell decrements seller's;
// unrelated slots and suits are unchanged.
TEST_CASE("Accumulation L1: buy/sell accounting is conservative", "[accumulation][L1]") {
    AccumulationEvalModule mod;
    GameStateSnapshot snap = make_acc_snap();
    mod.on_round_start(snap);

    mod.on_trade_event(make_trade(0, 1, Suit::Clubs, 0));

    REQUIRE_THAT(mod.signed_delta(0, suit_index(Suit::Clubs)),
                 Catch::Matchers::WithinAbs(1.0, 1e-9));
    REQUIRE_THAT(mod.signed_delta(1, suit_index(Suit::Clubs)),
                 Catch::Matchers::WithinAbs(-1.0, 1e-9));
    REQUIRE_THAT(mod.signed_delta(2, suit_index(Suit::Clubs)),
                 Catch::Matchers::WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(mod.signed_delta(0, suit_index(Suit::Spades)),
                 Catch::Matchers::WithinAbs(0.0, 1e-9));
}

// L1-2: Accumulation — repeated buys in same suit accumulate additively.
TEST_CASE("Accumulation L1: repeated buys accumulate correctly", "[accumulation][L1]") {
    AccumulationEvalModule mod;
    GameStateSnapshot snap = make_acc_snap();
    mod.on_round_start(snap);

    drive_buys(mod, Suit::Hearts, 5);

    REQUIRE_THAT(mod.signed_delta(0, suit_index(Suit::Hearts)),
                 Catch::Matchers::WithinAbs(5.0, 1e-9));
    REQUIRE_THAT(mod.signed_delta(1, suit_index(Suit::Hearts)),
                 Catch::Matchers::WithinAbs(-5.0, 1e-9));
}

// L1-3: Exact EWMA recurrence — ewma_next = alpha * |sample| + (1 - alpha) * ewma_prev.
// alpha = 0.1 (module default); after 1 Clubs trade: ewma = 0.1 * 1.0 = 0.1.
TEST_CASE("Accumulation L1: EWMA follows exact recurrence after one trade", "[accumulation][L1]") {
    AccumulationEvalModule mod;
    GameStateSnapshot snap = make_acc_snap();
    mod.on_round_start(snap);

    mod.on_trade_event(make_trade(0, 1, Suit::Clubs, 0));

    REQUIRE_THAT(mod.ewma_baseline_for(suit_index(Suit::Clubs)),
                 Catch::Matchers::WithinAbs(0.1, 1e-9));
}

// L1-4: Exact EWMA recurrence over two consecutive trades.
// After trade 1: ewma = 0.1.  After trade 2: ewma = 0.1*1 + 0.9*0.1 = 0.19.
TEST_CASE("Accumulation L1: EWMA recurrence correct over two trades", "[accumulation][L1]") {
    AccumulationEvalModule mod;
    GameStateSnapshot snap = make_acc_snap();
    mod.on_round_start(snap);

    mod.on_trade_event(make_trade(0, 1, Suit::Clubs, 0));
    mod.on_trade_event(make_trade(0, 1, Suit::Clubs, 200));

    REQUIRE_THAT(mod.ewma_baseline_for(suit_index(Suit::Clubs)),
                 Catch::Matchers::WithinAbs(0.19, 1e-9));
}

// L1-5: EWMA is suit-local — Clubs trades must not update Spades baseline.
TEST_CASE("Accumulation L1: EWMA is suit-isolated", "[accumulation][L1]") {
    AccumulationEvalModule mod;
    GameStateSnapshot snap = make_acc_snap();
    mod.on_round_start(snap);

    drive_buys(mod, Suit::Clubs, 3);

    const double clubs_ewma  = mod.ewma_baseline_for(suit_index(Suit::Clubs));
    const double spades_ewma = mod.ewma_baseline_for(suit_index(Suit::Spades));

    REQUIRE(clubs_ewma > 0.0);
    REQUIRE_THAT(spades_ewma, Catch::Matchers::WithinAbs(0.0, 1e-9));
}

// L1-6: Round reset clears signed deltas.
// First confirm a trade is recorded, then verify on_round_start zeroes it.
TEST_CASE("Accumulation L1: on_round_start resets signed deltas", "[accumulation][L1]") {
    AccumulationEvalModule mod;
    GameStateSnapshot snap = make_acc_snap();
    mod.on_round_start(snap);
    mod.on_trade_event(make_trade(0, 1, Suit::Spades, 0));

    REQUIRE(mod.signed_delta(0, suit_index(Suit::Spades)) > 0.0);

    mod.on_round_start(snap);

    REQUIRE_THAT(mod.signed_delta(0, suit_index(Suit::Spades)),
                 Catch::Matchers::WithinAbs(0.0, 1e-9));
}

// L1-7: Idempotency — calling on_round_end twice without new trades produces
// identical confidence values for every slot.
TEST_CASE("Accumulation L1: on_round_end is idempotent (no state drift)", "[accumulation][L1]") {
    GameStateSnapshot snap = make_acc_snap();

    std::vector<EvalOutput> out1, out2;
    AccumulationEvalModule mod;

    mod.on_round_start(snap);
    drive_buys(mod, Suit::Clubs, 3);

    mod.set_output_cb([&](EvalOutput o) { out1.push_back(std::move(o)); });
    mod.on_round_end(snap);

    mod.set_output_cb([&](EvalOutput o) { out2.push_back(std::move(o)); });
    mod.on_round_end(snap);

    REQUIRE(!out1.empty());
    REQUIRE(!out2.empty());

    const auto find_conf = [](const std::vector<EvalOutput>& outs, int slot) {
        for (const auto& o : outs)
            if (o.type == EvalOutput::Type::AccumulationSignal)
                for (const auto& p : o.payload["players"])
                    if (p["slot"].get<int>() == slot)
                        return p["confidence"].get<double>();
        return -1.0;
    };

    for (int s = 0; s < 4; ++s)
        REQUIRE_THAT(find_conf(out1, s),
                     Catch::Matchers::WithinAbs(find_conf(out2, s), 1e-12));
}

// L1-8: Symmetry — same trade pattern applied to two different suits must
// produce equal absolute delta magnitudes.
TEST_CASE("Accumulation L1: equal trade patterns in different suits yield equal magnitudes",
          "[accumulation][L1]") {
    auto delta_after_5_buys = [](Suit suit) {
        AccumulationEvalModule mod;
        GameStateSnapshot snap = make_acc_snap();
        mod.on_round_start(snap);
        for (int i = 0; i < 5; ++i)
            mod.on_trade_event(make_trade(0, 1, suit, i * 200LL));
        return mod.signed_delta(0, suit_index(suit));
    };

    const double d_clubs  = delta_after_5_buys(Suit::Clubs);
    const double d_spades = delta_after_5_buys(Suit::Spades);

    REQUIRE(d_clubs > 0.0);
    REQUIRE_THAT(d_clubs, Catch::Matchers::WithinAbs(d_spades, 1e-9));
}

// L1-9: Neutral flow — balanced buy/sell in same suit keeps slot 0 at Normal.
TEST_CASE("Accumulation L1: balanced buy/sell keeps signal Normal", "[accumulation][L1]") {
    std::vector<EvalOutput> outputs;
    AccumulationEvalModule mod;
    mod.set_output_cb([&](EvalOutput o) { outputs.push_back(std::move(o)); });

    GameStateSnapshot snap = make_acc_snap();
    mod.on_round_start(snap);
    for (int i = 0; i < 5; ++i) {
        mod.on_trade_event(make_trade(0, 1, Suit::Clubs, i * 400LL));
        mod.on_trade_event(make_trade(1, 0, Suit::Clubs, i * 400LL + 200));
    }
    mod.on_round_end(snap);

    REQUIRE(!outputs.empty());

    for (const auto& out : outputs) {
        if (out.type != EvalOutput::Type::AccumulationSignal) continue;
        for (const auto& player : out.payload["players"])
            if (player["slot"].get<int>() == 0)
                REQUIRE(player["signal"].get<std::string>() == "Normal");
    }
}

// L1-10: Confidence monotonicity — more concentrated one-sided accumulation
// must never lower confidence.
TEST_CASE("Accumulation L1: confidence is monotone in accumulation intensity",
          "[accumulation][L1]") {
    const double c2  = confidence_after_n_buys(2);
    const double c5  = confidence_after_n_buys(5);
    const double c10 = confidence_after_n_buys(10);

    REQUIRE(c2  >= 0.0);
    REQUIRE(c5  >= c2);
    REQUIRE(c10 >= c5);
}

// L1-11: Noise robustness — perfectly balanced cross-suit trading must not
// produce a High signal for any player.
TEST_CASE("Accumulation L1: balanced noise does not trigger High signal",
          "[accumulation][L1]") {
    std::vector<EvalOutput> outputs;
    AccumulationEvalModule mod;
    mod.set_output_cb([&](EvalOutput o) { outputs.push_back(std::move(o)); });

    GameStateSnapshot snap = make_acc_snap();
    mod.on_round_start(snap);
    // Each slot buys and sells each suit exactly once — perfectly balanced
    for (int s = 0; s < 4; ++s)
        for (int buyer = 0; buyer < 4; ++buyer)
            mod.on_trade_event(make_trade(buyer, (buyer + 1) % 4, kAllSuits[s],
                                          (s * 4 + buyer) * 150LL));
    mod.on_round_end(snap);

    REQUIRE(!outputs.empty());

    for (const auto& out : outputs) {
        if (out.type != EvalOutput::Type::AccumulationSignal) continue;
        for (const auto& player : out.payload["players"])
            REQUIRE(player["signal"].get<std::string>() != "High");
    }
}

// L1-12: Numerical safety — no NaN, Inf, negative, or un-processed trades
// after a 1000-trade stress run.
TEST_CASE("Accumulation L1: no NaN/Inf/negative under 1000-trade stress",
          "[accumulation][L1]") {
    std::vector<EvalOutput> outputs;
    AccumulationEvalModule mod;
    mod.set_output_cb([&](EvalOutput o) { outputs.push_back(std::move(o)); });

    GameStateSnapshot snap = make_acc_snap();
    mod.on_round_start(snap);

    for (int i = 0; i < 1000; ++i)
        mod.on_trade_event(make_trade(i % 4, (i + 1) % 4, kAllSuits[i % 4], i * 50LL));

    mod.on_round_end(snap);

    double total_abs_delta = 0.0;
    for (int slot = 0; slot < 4; ++slot)
        for (int s = 0; s < 4; ++s) {
            const double d = mod.signed_delta(slot, s);
            REQUIRE(std::isfinite(d));
            total_abs_delta += std::abs(d);
        }
    REQUIRE(total_abs_delta > 0.0);

    for (int s = 0; s < 4; ++s) {
        const double b = mod.ewma_baseline_for(s);
        REQUIRE(std::isfinite(b));
        REQUIRE(b >= 0.0);
    }

    REQUIRE(!outputs.empty());
    for (const auto& out : outputs) {
        if (out.type != EvalOutput::Type::AccumulationSignal) continue;
        for (const auto& player : out.payload["players"]) {
            const double conf = player["confidence"].get<double>();
            REQUIRE(std::isfinite(conf));
            REQUIRE(conf >= 0.0);
            REQUIRE(conf <= 1.0);
        }
    }
}

// L1-13: Zero-trade round must produce all Normal signals.
TEST_CASE("Accumulation L1: zero-trade round produces all Normal signals",
          "[accumulation][L1]") {
    std::vector<EvalOutput> outputs;
    AccumulationEvalModule mod;
    mod.set_output_cb([&](EvalOutput o) { outputs.push_back(std::move(o)); });

    GameStateSnapshot snap = make_acc_snap();
    mod.on_round_start(snap);
    mod.on_round_end(snap);

    REQUIRE(!outputs.empty());

    for (const auto& out : outputs) {
        if (out.type != EvalOutput::Type::AccumulationSignal) continue;
        for (const auto& player : out.payload["players"])
            REQUIRE(player["signal"].get<std::string>() == "Normal");
    }
}

// L1-14: Extreme one-sided accumulation — 20 consecutive uncontested buys must
// produce High signal.  Far above any reasonable Elevated/High boundary.
TEST_CASE("Accumulation L1: extreme one-sided accumulation produces High signal",
          "[accumulation][L1]") {
    std::vector<EvalOutput> outputs;
    AccumulationEvalModule mod;
    mod.set_output_cb([&](EvalOutput o) { outputs.push_back(std::move(o)); });

    GameStateSnapshot snap = make_acc_snap();
    mod.on_round_start(snap);
    drive_buys(mod, Suit::Diamonds, 20);
    mod.on_round_end(snap);

    REQUIRE(!outputs.empty());

    bool found_high = false;
    for (const auto& out : outputs) {
        if (out.type != EvalOutput::Type::AccumulationSignal) continue;
        for (const auto& player : out.payload["players"])
            if (player["slot"].get<int>() == 0 &&
                player["signal"].get<std::string>() == "High")
                found_high = true;
    }
    REQUIRE(found_high);
}

// ===========================================================================
// LAYER 2 — Behavioral monotonicity and stability
// ===========================================================================

// L2-1: Strong accumulation dominance — aggressively buying one suit must
// identify that suit as primary_suit in the output.
TEST_CASE("Accumulation L2: aggressive buying identifies primary suit correctly",
          "[accumulation][L2]") {
    std::vector<EvalOutput> outputs;
    AccumulationEvalModule mod;
    mod.set_output_cb([&](EvalOutput o) { outputs.push_back(std::move(o)); });

    GameStateSnapshot snap = make_acc_snap();
    mod.on_round_start(snap);
    drive_buys(mod, Suit::Hearts, 10);
    mod.on_round_end(snap);

    REQUIRE(!outputs.empty());

    bool found = false;
    for (const auto& out : outputs) {
        if (out.type != EvalOutput::Type::AccumulationSignal) continue;
        for (const auto& player : out.payload["players"]) {
            if (player["slot"].get<int>() == 0) {
                found = true;
                REQUIRE(player["primary_suit"].get<std::string>() == "hearts");
            }
        }
    }
    REQUIRE(found);
}

// L2-2: Relative strength ordering — 3, 8, 20 buys must produce monotone
// non-decreasing confidence.
TEST_CASE("Accumulation L2: signal strength orders monotonically with accumulation",
          "[accumulation][L2]") {
    const double c3  = confidence_after_n_buys(3);
    const double c8  = confidence_after_n_buys(8);
    const double c20 = confidence_after_n_buys(20);

    REQUIRE(c3  >= 0.0);
    REQUIRE(c8  >= c3);
    REQUIRE(c20 >= c8);
}

// L2-3: Early-vs-late consistency — identical trade patterns at different
// timestamps must produce the same primary suit conclusion.
TEST_CASE("Accumulation L2: trade timing does not invert primary suit conclusion",
          "[accumulation][L2]") {
    auto get_primary = [](int64_t ts_offset) {
        std::vector<EvalOutput> outputs;
        AccumulationEvalModule mod;
        mod.set_output_cb([&](EvalOutput o) { outputs.push_back(std::move(o)); });
        GameStateSnapshot snap = make_acc_snap();
        mod.on_round_start(snap);
        for (int i = 0; i < 5; ++i)
            mod.on_trade_event(make_trade(0, 1, Suit::Spades, ts_offset + i * 200LL));
        mod.on_round_end(snap);
        for (const auto& out : outputs)
            if (out.type == EvalOutput::Type::AccumulationSignal)
                for (const auto& p : out.payload["players"])
                    if (p["slot"].get<int>() == 0)
                        return p["primary_suit"].get<std::string>();
        return std::string{};
    };

    const auto early = get_primary(0);
    const auto late  = get_primary(60000);

    REQUIRE(!early.empty());
    REQUIRE(early == late);
}

// L2-4: Opposing accumulation cancellation — alternating buy/sell by slot 0
// must produce lower confidence than pure one-sided buying.
TEST_CASE("Accumulation L2: opposing accumulation reduces confidence",
          "[accumulation][L2]") {
    const double conf_pure = confidence_after_n_buys(5, Suit::Clubs);

    std::vector<EvalOutput> outputs;
    AccumulationEvalModule mod;
    mod.set_output_cb([&](EvalOutput o) { outputs.push_back(std::move(o)); });
    GameStateSnapshot snap = make_acc_snap();
    mod.on_round_start(snap);
    for (int i = 0; i < 5; ++i) {
        mod.on_trade_event(make_trade(0, 1, Suit::Clubs, i * 400LL));
        mod.on_trade_event(make_trade(1, 0, Suit::Clubs, i * 400LL + 200));
    }
    mod.on_round_end(snap);

    double conf_alternating = -1.0;
    for (const auto& out : outputs)
        if (out.type == EvalOutput::Type::AccumulationSignal)
            for (const auto& p : out.payload["players"])
                if (p["slot"].get<int>() == 0)
                    conf_alternating = p["confidence"].get<double>();

    REQUIRE(conf_pure >= 0.0);
    REQUIRE(conf_alternating >= 0.0);
    REQUIRE(conf_pure > conf_alternating);
}

// L2-5: Multi-player independence — heavy accumulation by slot 0 must not
// raise the signal of slot 2, which participates in no trades.
TEST_CASE("Accumulation L2: one player's flow does not contaminate silent players",
          "[accumulation][L2]") {
    std::vector<EvalOutput> outputs;
    AccumulationEvalModule mod;
    mod.set_output_cb([&](EvalOutput o) { outputs.push_back(std::move(o)); });

    GameStateSnapshot snap = make_acc_snap();
    mod.on_round_start(snap);
    drive_buys(mod, Suit::Clubs, 15);
    mod.on_round_end(snap);

    REQUIRE(!outputs.empty());

    bool found = false;
    for (const auto& out : outputs) {
        if (out.type != EvalOutput::Type::AccumulationSignal) continue;
        for (const auto& player : out.payload["players"]) {
            if (player["slot"].get<int>() == 2) {
                found = true;
                REQUIRE(player["signal"].get<std::string>() == "Normal");
            }
        }
    }
    REQUIRE(found);
}

// L2-6: Sparse-data uncertainty — a single trade must not produce a High signal.
TEST_CASE("Accumulation L2: single trade produces low confidence", "[accumulation][L2]") {
    std::vector<EvalOutput> outputs;
    AccumulationEvalModule mod;
    mod.set_output_cb([&](EvalOutput o) { outputs.push_back(std::move(o)); });

    GameStateSnapshot snap = make_acc_snap();
    mod.on_round_start(snap);
    mod.on_trade_event(make_trade(0, 1, Suit::Diamonds, 0));
    mod.on_round_end(snap);

    REQUIRE(!outputs.empty());

    for (const auto& out : outputs) {
        if (out.type != EvalOutput::Type::AccumulationSignal) continue;
        for (const auto& player : out.payload["players"])
            if (player["slot"].get<int>() == 0)
                REQUIRE(player["signal"].get<std::string>() != "High");
    }
}

// L2-7: Replay determinism — identical event streams produce bit-identical outputs.
TEST_CASE("Accumulation L2: identical event streams produce deterministic outputs",
          "[accumulation][L2]") {
    auto run = []() {
        std::vector<EvalOutput> outputs;
        AccumulationEvalModule mod;
        mod.set_output_cb([&](EvalOutput o) { outputs.push_back(std::move(o)); });
        GameStateSnapshot snap = make_acc_snap();
        mod.on_round_start(snap);
        mod.on_trade_event(make_trade(0, 1, Suit::Clubs,    500));
        mod.on_trade_event(make_trade(2, 3, Suit::Diamonds, 1000));
        mod.on_trade_event(make_trade(0, 2, Suit::Clubs,    1500));
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

// ===========================================================================
// Performance — latency bounds with CI-safe thresholds
// ===========================================================================

// PERF-1: on_round_start < 0.5 ms average over 500 iterations.
TEST_CASE("Accumulation perf: on_round_start < 0.5ms average", "[accumulation][perf]") {
    AccumulationEvalModule mod;
    GameStateSnapshot snap = make_acc_snap();

    const int kRuns = 500;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kRuns; ++i)
        mod.on_round_start(snap);
    auto t1 = std::chrono::steady_clock::now();

    const double avg_us =
        std::chrono::duration<double, std::micro>(t1 - t0).count() / kRuns;
    REQUIRE(avg_us < 500.0);
}

// PERF-2: Full round pipeline (round_start + 50 trades + round_end) < 3 ms.
TEST_CASE("Accumulation perf: full round pipeline (start+50 trades+end) < 3ms",
          "[accumulation][perf]") {
    AccumulationEvalModule mod;
    mod.set_output_cb([](EvalOutput) {});
    GameStateSnapshot snap = make_acc_snap();

    auto t0 = std::chrono::steady_clock::now();
    mod.on_round_start(snap);
    for (int i = 0; i < 50; ++i)
        mod.on_trade_event(make_trade(i % 4, (i + 1) % 4, kAllSuits[i % 4], i * 100LL));
    mod.on_round_end(snap);
    auto t1 = std::chrono::steady_clock::now();

    const double elapsed_us =
        std::chrono::duration<double, std::micro>(t1 - t0).count();
    REQUIRE(elapsed_us < 3000.0);
}

// PERF-3: High-volume burst — 1000 on_trade_event calls < 20 ms total.
// Validates O(1) per-trade asymptotic behaviour.
TEST_CASE("Accumulation perf: 1000-trade burst < 20ms total", "[accumulation][perf]") {
    AccumulationEvalModule mod;
    mod.set_output_cb([](EvalOutput) {});
    GameStateSnapshot snap = make_acc_snap();
    mod.on_round_start(snap);

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i)
        mod.on_trade_event(make_trade(i % 4, (i + 1) % 4, kAllSuits[i % 4], i * 50LL));
    auto t1 = std::chrono::steady_clock::now();

    const double elapsed_us =
        std::chrono::duration<double, std::micro>(t1 - t0).count();
    REQUIRE(elapsed_us < 20000.0);
}
