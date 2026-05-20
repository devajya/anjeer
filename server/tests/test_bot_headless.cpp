#include <catch2/catch_test_macros.hpp>

#include "server/game_session.h"
#include "server/bot_manager.h"
#include "server/bot_scheduler.h"
#include "server/config.h"
#include "server/db.h"
#include "server/logger.h"
#include "server/session_queue.h"

#include <readerwriterqueue.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include <atomic>
#include <chrono>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace anjeer::server;
using namespace anjeer::engine;
using namespace std::chrono_literals;

// ── Test infrastructure ────────────────────────────────────────────────────────

static DbPool& test_db() {
    static DbPool pool(TEST_DB_CONN, 2);
    return pool;
}

static void run_migrations() {
    static bool done = false;
    if (done) return;
    pqxx::connection conn(TEST_DB_CONN);
    DbMigrator m(conn, TEST_MIGRATIONS_DIR);
    m.run();
    done = true;
}

static ServerConfig make_headless_cfg(int n_players) {
    ServerConfig cfg;
    cfg.host                  = "127.0.0.1";
    cfg.port                  = 9999;
    cfg.heartbeat_interval_ms = 60000;
    cfg.ping_interval_ms      = 1000;
    cfg.ping_timeout_ms       = 8000;
    cfg.order_book.min_price               = 1;
    cfg.order_book.max_price               = 99;
    cfg.order_book.nudge_initial_buy_price  = 1;
    cfg.order_book.nudge_initial_sell_price = 99;
    cfg.order_book.active_suits             = {"clubs","diamonds","hearts","spades"};
    cfg.game.player_count           = n_players;
    cfg.game.total_cards            = 40;
    cfg.game.countdown_seconds      = 0;
    cfg.game.round_duration_seconds = 8;
    cfg.game.inter_round_seconds    = 1;
    cfg.scoring.starting_balance    = 1000;
    cfg.scoring.pot_size            = 500;
    cfg.scoring.points_per_card     = 20;
    cfg.lobby.min_players           = 2;
    cfg.lobby.max_players           = 8;
    cfg.db.connection_string        = TEST_DB_CONN;
    cfg.db.pool_size                = 2;
    cfg.bots.scheduler_threads    = 4;
    cfg.bots.scheduler_tick_ms    = 50;
    cfg.bots.sim_network_delay_ms = 0;

    auto fill_dp = [](ServerConfig::BotsConfig::PerDifficultyParams& p,
                      int tick_ms, int max_orders) {
        p.tick_interval_ms      = tick_ms;
        p.tick_jitter_ms        = 0;
        p.thinking_min_ms       = 0;
        p.thinking_max_ms       = 0;
        p.confidence_discount   = 0.90f;
        p.taker_threshold       = 0.95f;
        p.min_bid_ev            = 2.0f;
        p.max_ask_ev            = 8.0f;
        p.hand_size_cap         = 5;
        p.offload_threshold     = 4;
        p.max_concurrent_orders = max_orders;
        p.conviction_threshold  = 0.70f;
        p.max_resting_ms        = 3000;
        p.nudge_probability     = 0.0f;
        p.nudge_patience_ms     = 1500;
        p.nudge_max_gap         = 2;
        p.endgame_threshold_s   = 0;
        p.early_seed_threshold  = 0.0f;
    };
    fill_dp(cfg.bots.easy,   100, 2);
    fill_dp(cfg.bots.medium,  50, 4);
    fill_dp(cfg.bots.hard,    20, 6);
    return cfg;
}

static std::string make_session_in_db(const std::string& lobby_id) {
    try {
        auto conn = test_db().acquire();
        pqxx::work txn(conn.get());
        const auto r = txn.exec_params(
            "INSERT INTO game_sessions (lobby_id) VALUES ($1) RETURNING id",
            lobby_id);
        std::string id = r[0][0].as<std::string>();
        txn.commit();
        return id;
    } catch (...) {
        return "headless-session-" + lobby_id;
    }
}

// ── T20 ───────────────────────────────────────────────────────────────────────

TEST_CASE("headless sim: 5 bots play a full game to completion", "[bots][headless]") {
    run_migrations();

    constexpr int N = 5;
    const std::string lobby_id   = "headless-lobby-t20";
    const std::string session_id = make_session_in_db(lobby_id);

    const ServerConfig cfg = make_headless_cfg(N);

    Logger server_log("logs/headless_server.txt");
    Logger engine_log("logs/headless_engine.txt");
    std::mt19937 rng(42);

    std::vector<SlotInfo> slots;
    for (int i = 0; i < N; ++i) {
        SlotInfo s;
        s.player_id = -(static_cast<int64_t>(i) + 1);
        s.username  = "HeadlessBot" + std::to_string(i);
        s.balance   = cfg.scoring.starting_balance;
        s.connected = false;
        s.active    = true;
        slots.push_back(s);
    }

    moodycamel::ReaderWriterQueue<NetEvent>  inbound{512};
    moodycamel::ReaderWriterQueue<GameEvent> outbound{512};

    BotScheduler sched(cfg.bots.scheduler_threads);
    BotManager   mgr(sched, cfg.bots);

    // Register bots and build slot_map
    std::unordered_map<std::string, int> slot_map;
    for (int i = 0; i < N; ++i) {
        auto res = mgr.add_bot(lobby_id, BotDifficulty::Easy, N - i);
        REQUIRE(res.ok);
        slot_map[res.bot_uuid] = i;
    }

    // Attach adapters and enqueue NetConnect for each bot before NetStartGame
    mgr.attach_to_session(
        lobby_id, slot_map,
        cfg.scoring.points_per_card,
        cfg.scoring.pot_size / N,
        cfg.game.round_duration_seconds
    );
    for (int i = 0; i < N; ++i)
        inbound.enqueue(NetConnect{i, -(static_cast<int64_t>(i) + 1), slots[i].username});

    GameSession session(
        session_id, lobby_id, slots,
        GameSessionContext{cfg, server_log, engine_log, rng, test_db()},
        inbound, outbound);
    session.start();
    inbound.enqueue(NetStartGame{});

    // Fake drain loop: mirrors WsServer::drain_all_on_loop for bot-only sessions.
    // On inter_round, all bot slots vote to end so the game terminates after one round.
    const auto deadline = std::chrono::steady_clock::now() + 120s;
    bool game_ended = false;

    while (!game_ended && std::chrono::steady_clock::now() < deadline) {
        GameEvent ev;
        while (outbound.try_dequeue(ev)) {
            std::visit([&](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, GameBroadcast>) {
                    mgr.dispatch_to_bots(lobby_id, arg.json, -1);
                    try {
                        auto j = nlohmann::json::parse(arg.json);
                        if (j.value("type", "") == "inter_round") {
                            inbound.enqueue(NetOwnerEndGame{});
                        }
                    } catch (...) {}
                } else if constexpr (std::is_same_v<T, GameTargeted>) {
                    mgr.dispatch_to_bots(lobby_id, arg.json, arg.slot);
                } else if constexpr (std::is_same_v<T, GameDone>) {
                    game_ended = true;
                }
            }, ev);
        }
        mgr.drain_bot_actions(lobby_id, inbound);
        std::this_thread::sleep_for(8ms);
    }

    REQUIRE(game_ended);
    mgr.teardown_session(lobby_id);
    session.shutdown();
}

// ── T21 ───────────────────────────────────────────────────────────────────────

TEST_CASE("BotScheduler ceiling: 20 concurrent bots use at most 4 threads", "[bots][headless]") {
    BotScheduler sched(4);
    REQUIRE(sched.thread_count() == 4);

    std::atomic<int> current{0};
    std::atomic<int> peak{0};

    for (int i = 0; i < 20; ++i) {
        sched.register_bot([&] {
            int c = ++current;
            int expected = peak.load();
            while (c > expected && !peak.compare_exchange_weak(expected, c));
            std::this_thread::sleep_for(5ms);
            --current;
        }, 50);
    }

    std::this_thread::sleep_for(300ms);

    REQUIRE(sched.thread_count() == 4);
    REQUIRE(peak.load() <= 4);
}
