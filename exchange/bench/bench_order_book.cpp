// Throughput harness for OrderBook. Not a pass/fail gate — it prints numbers.
// Build/run with `make bench-exchange` (see Makefile). PERF_N sets the op count.
//
// Every scenario pre-generates its inputs before the timed region so the clock
// only covers OrderBook work, never RNG or allocation of the input tape.

#include "exchange/order_book.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <variant>
#include <vector>

using namespace anjeer::exchange;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kReps = 5;
constexpr int32_t kMinPrice = 1;
constexpr int32_t kMaxPrice = 99;

// Accumulator that keeps the optimizer from deleting work whose results are
// otherwise unused. Printed at the end so it is observably live.
volatile uint64_t g_sink = 0;

OrderBook::Config make_config(const std::string& suit) {
    OrderBook::Config cfg;
    cfg.min_price = kMinPrice;
    cfg.max_price = kMaxPrice;
    cfg.nudge_initial_buy_price = kMinPrice;
    cfg.nudge_initial_sell_price = kMaxPrice;
    cfg.suit = suit;
    return cfg;
}

uint64_t consume(const std::vector<OrderEvent>& events) {
    uint64_t acc = events.size();
    for (const auto& ev : events) {
        if (const auto* t = std::get_if<TradeEvent>(&ev)) acc += static_cast<uint64_t>(t->price);
    }
    return acc;
}

const OrderAckEvent* find_ack(const std::vector<OrderEvent>& events) {
    for (const auto& ev : events) {
        if (const auto* a = std::get_if<OrderAckEvent>(&ev)) return a;
    }
    return nullptr;
}

struct Result {
    std::string name;
    std::string note;
    int64_t     ops;
    double      ns_per_op;
    double      ops_per_sec;
};

std::vector<Result> g_results;

// Runs fn kReps+1 times (first is warmup) and keeps the fastest pass. Best-of
// is the right summary for a microbenchmark: slow passes are scheduler noise,
// they never mean the code got faster.
template <typename Fn>
void measure(const std::string& name, const std::string& note, int64_t ops, Fn fn) {
    double best_ns = 0.0;
    for (int rep = 0; rep <= kReps; ++rep) {
        const auto t0 = Clock::now();
        const uint64_t acc = fn();
        const auto t1 = Clock::now();
        g_sink += acc;
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        if (rep == 0) continue;  // warmup
        if (best_ns == 0.0 || ns < best_ns) best_ns = ns;
    }
    const double per_op = best_ns / static_cast<double>(ops);
    g_results.push_back({name, note, ops, per_op, 1e9 / per_op});
}

// ---------------------------------------------------------------------------
// Scenario 1 — game scale: 4 suits, 5 players, shallow books, wipe on trade.
// This is the shape the server actually drives, so it is the headline number.
// ---------------------------------------------------------------------------

struct Op {
    int      book;
    int32_t  player;
    Side     side;
    int32_t  price;
};

std::vector<Op> gen_game_ops(int64_t n) {
    std::mt19937 rng{12345};
    std::uniform_int_distribution<int> book_d{0, 3};
    std::uniform_int_distribution<int> player_d{1, 5};
    std::uniform_int_distribution<int> side_d{0, 1};
    std::uniform_int_distribution<int> price_d{kMinPrice, kMaxPrice};

    std::vector<Op> ops;
    ops.reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        ops.push_back({book_d(rng), static_cast<int32_t>(player_d(rng)),
                       side_d(rng) == 0 ? Side::Buy : Side::Sell,
                       static_cast<int32_t>(price_d(rng))});
    }
    return ops;
}

uint64_t run_game_scale(const std::vector<Op>& ops) {
    std::vector<OrderBook> books;
    books.reserve(4);
    for (int i = 0; i < 4; ++i) books.emplace_back(make_config("S" + std::to_string(i + 1)));

    uint64_t acc = 0;
    for (const auto& op : ops) {
        auto events = books[static_cast<size_t>(op.book)].submit(op.player, op.side, op.price);
        acc += consume(events);

        bool traded = false;
        for (const auto& ev : events) {
            if (std::holds_alternative<TradeEvent>(ev)) { traded = true; break; }
        }
        // WIPE CONTRACT: a trade in any book wipes every book. Server policy,
        // reproduced here because it dominates steady-state book depth.
        if (traded) {
            for (auto& b : books) acc += consume(b.wipe());
        }
    }
    return acc;
}

// ---------------------------------------------------------------------------
// Scenario 2 — match heavy: every second order crosses immediately.
// Isolates the match + erase path with no wipe and no depth growth.
// ---------------------------------------------------------------------------

uint64_t run_match_heavy(int64_t n) {
    OrderBook book{make_config("S1")};
    uint64_t acc = 0;
    for (int64_t i = 0; i < n; i += 2) {
        acc += consume(book.submit(1, Side::Buy, 50));
        acc += consume(book.submit(2, Side::Sell, 50));  // crosses -> trade
    }
    return acc;
}

// ---------------------------------------------------------------------------
// Scenario 3 — depth sweep: steady-state book of D resting orders per side,
// then submit/cancel churn. Shows how the sorted-vector insert and the linear
// cancel scan scale as depth grows past the ~5 the design assumes.
// ---------------------------------------------------------------------------

void prefill(OrderBook& book, int depth) {
    for (int i = 0; i < depth; ++i) {
        g_sink += consume(book.submit(1, Side::Buy, 1 + (i % 40)));
        g_sink += consume(book.submit(2, Side::Sell, 60 + (i % 40)));
    }
}

uint64_t run_depth_churn(int depth, const std::vector<int32_t>& prices) {
    OrderBook book{make_config("S1")};
    prefill(book, depth);

    uint64_t acc = 0;
    for (const int32_t price : prices) {
        auto events = book.submit(3, Side::Buy, price);
        acc += consume(events);
        const OrderAckEvent* ack = find_ack(events);
        if (ack != nullptr) acc += consume(book.cancel(ack->order_id, 3));
    }
    return acc;
}

}  // namespace

int main(int argc, char** argv) {
    int64_t n = 200000;
    if (argc > 1) n = std::strtoll(argv[1], nullptr, 10);
    if (n < 1000) n = 1000;

    const std::vector<Op> game_ops = gen_game_ops(n);
    measure("game_scale", "4 books, 5 players, wipe on trade", n,
            [&] { return run_game_scale(game_ops); });

    measure("match_heavy", "every 2nd order crosses", n,
            [&] { return run_match_heavy(n); });

    // Churn prices stay under the resting asks so the submit never crosses —
    // this measures insert + cancel cost at depth, not the match path.
    std::mt19937 rng{999};
    std::uniform_int_distribution<int> churn_d{1, 40};
    const int64_t churn_n = n / 4;
    std::vector<int32_t> churn_prices;
    churn_prices.reserve(static_cast<size_t>(churn_n));
    for (int64_t i = 0; i < churn_n; ++i) churn_prices.push_back(static_cast<int32_t>(churn_d(rng)));

    for (const int depth : {4, 16, 64, 256}) {
        measure("depth_churn_" + std::to_string(depth),
                "submit+cancel at depth " + std::to_string(depth) + "/side", churn_n,
                [&] { return run_depth_churn(depth, churn_prices); });
    }

    std::printf("\nOrderBook throughput  (n=%lld, best of %d)\n", static_cast<long long>(n), kReps);
    std::printf("%-20s %12s %14s   %s\n", "scenario", "ns/order", "orders/sec", "workload");
    std::printf("%-20s %12s %14s   %s\n", "--------------------", "------------",
                "--------------", "--------");
    for (const auto& r : g_results) {
        std::printf("%-20s %12.1f %14.0f   %s\n", r.name.c_str(), r.ns_per_op, r.ops_per_sec,
                    r.note.c_str());
    }
    std::printf("\nchecksum %llu\n", static_cast<unsigned long long>(g_sink));
    return 0;
}
