#include <catch2/catch_test_macros.hpp>

#include "server/game_session.h"
#include "server/game_session_wire.h"
#include "server/session_queue.h"
#include "server/config.h"
#include "server/db.h"
#include "server/logger.h"

#include <readerwriterqueue.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include <chrono>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace anjeer::server;
using namespace anjeer::engine;
using namespace std::chrono_literals;

// ─── Shared infrastructure ────────────────────────────────────────────────────

static DbPool& test_db() {
    static DbPool pool(TEST_DB_CONN, 1);
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

static ServerConfig make_cfg() {
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
    cfg.order_book.active_suits             = {"clubs"};
    cfg.game.player_count           = 2;
    cfg.game.total_cards            = 40;
    cfg.game.countdown_seconds      = 0;
    cfg.game.round_duration_seconds = 60;
    cfg.game.inter_round_seconds    = 60;
    cfg.scoring.starting_balance    = 1000;
    cfg.scoring.pot_size            = 200;
    cfg.scoring.points_per_card     = 20;
    cfg.lobby.min_players           = 2;
    cfg.lobby.max_players           = 8;
    cfg.db.connection_string        = TEST_DB_CONN;
    cfg.db.pool_size                = 1;
    return cfg;
}

static std::vector<SlotInfo> make_slots(int n, int balance = 1000) {
    std::vector<SlotInfo> slots;
    for (int i = 0; i < n; ++i) {
        SlotInfo s;
        s.player_id = static_cast<int64_t>(i + 1);
        s.username  = "player" + std::to_string(i);
        s.balance   = balance;
        s.connected = false;
        s.active    = true;
        slots.push_back(s);
    }
    return slots;
}

// Create a session_id in DB so foreign-key constraints don't fail.
static std::string make_session_in_db(const std::string& lobby_id) {
    try {
        auto conn = test_db().acquire();
        pqxx::work txn(conn.get());
        const auto r = txn.exec_params(
            "INSERT INTO game_sessions (lobby_id) VALUES ($1) RETURNING id",
            lobby_id
        );
        const std::string id = r[0][0].as<std::string>();
        txn.commit();
        return id;
    } catch (...) {
        return "test-session-" + lobby_id;
    }
}

// ─── Test harness ─────────────────────────────────────────────────────────────

struct Harness {
    moodycamel::ReaderWriterQueue<NetEvent>  inbound{256};
    moodycamel::ReaderWriterQueue<GameEvent> outbound{256};
    Logger  server_log;
    Logger  engine_log;
    std::mt19937 rng{42};
    ServerConfig cfg;
    std::unique_ptr<GameSession> session;

    explicit Harness(ServerConfig c = make_cfg(),
                     std::vector<SlotInfo> slots = make_slots(2),
                     const std::string& session_id = "test-session",
                     const std::string& lobby_id   = "test-lobby")
        : server_log("logs/gs_test_server.txt")
        , engine_log("logs/gs_test_engine.txt")
        , cfg(std::move(c))
    {
        session = std::make_unique<GameSession>(
            session_id, lobby_id, std::move(slots),
            GameSessionContext{cfg, server_log, engine_log, rng, test_db()},
            inbound, outbound);
        session->start();
    }

    ~Harness() { session->shutdown(); }

    void push(NetEvent ev) { inbound.enqueue(std::move(ev)); }

    // Poll outbound until a GameBroadcast or GameTargeted with the given JSON
    // "type" field arrives, or the timeout elapses.
    std::optional<nlohmann::json> recv_type(
            const std::string& type,
            std::chrono::milliseconds timeout = 2000ms) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            GameEvent ev;
            if (outbound.try_dequeue(ev)) {
                auto json_str = std::visit([](auto&& e) -> std::string {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, GameBroadcast>) return e.json;
                    if constexpr (std::is_same_v<T, GameTargeted>)  return e.json;
                    return "";
                }, ev);
                if (json_str.empty()) continue;
                auto j = nlohmann::json::parse(json_str);
                if (j.value("type", "") == type) return j;
            } else {
                std::this_thread::sleep_for(10ms);
            }
        }
        return std::nullopt;
    }

    // Recv targeted at a specific slot with the given type.
    std::optional<nlohmann::json> recv_targeted(
            int32_t slot,
            const std::string& type,
            std::chrono::milliseconds timeout = 2000ms) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            GameEvent ev;
            if (outbound.try_dequeue(ev)) {
                if (const auto* t = std::get_if<GameTargeted>(&ev)) {
                    if (t->slot == slot) {
                        auto j = nlohmann::json::parse(t->json);
                        if (j.value("type", "") == type) return j;
                    }
                }
            } else {
                std::this_thread::sleep_for(10ms);
            }
        }
        return std::nullopt;
    }

    bool recv_done(std::chrono::milliseconds timeout = 2000ms) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            GameEvent ev;
            if (outbound.try_dequeue(ev)) {
                if (std::holds_alternative<GameDone>(ev)) return true;
            } else {
                std::this_thread::sleep_for(10ms);
            }
        }
        return false;
    }

    // Connect both slots.
    void connect_all() {
        push(NetConnect{0, 1, "player0"});
        push(NetConnect{1, 2, "player1"});
    }

    // Connect, start game, wait for round_start on slot 0.
    void advance_to_round_active() {
        connect_all();
        push(NetStartGame{});
        REQUIRE(recv_type("round_starting").has_value());
        REQUIRE(recv_targeted(0, "round_start").has_value());
        REQUIRE(recv_targeted(1, "round_start").has_value());
    }
};

// ─── G1: Lobby phase — waiting_for_start on connect ──────────────────────────

TEST_CASE("G1: waiting_for_start emitted when player connects in Lobby phase",
          "[game_session]") {
    run_migrations();
    Harness h;
    h.push(NetConnect{0, 1, "player0"});
    const auto msg = h.recv_type("waiting_for_start");
    REQUIRE(msg.has_value());
    CHECK(msg->value("connected", -1) == 1);
    CHECK(msg->value("required",  -1) == 2);
}

// ─── G2: Countdown → round_starting broadcast ─────────────────────────────────

TEST_CASE("G2: start_game in Lobby phase broadcasts round_starting",
          "[game_session]") {
    run_migrations();
    Harness h;
    h.connect_all();
    h.push(NetStartGame{});
    const auto msg = h.recv_type("round_starting");
    REQUIRE(msg.has_value());
    CHECK(msg->contains("starts_at"));
    CHECK(msg->value("player_count", -1) == 2);
}

// ─── G3: round_start targeted after countdown expires ─────────────────────────

TEST_CASE("G3: round_start targeted to each slot after countdown (0s)",
          "[game_session]") {
    run_migrations();
    Harness h;
    h.connect_all();
    h.push(NetStartGame{});
    h.recv_type("round_starting");

    const auto rs0 = h.recv_targeted(0, "round_start");
    const auto rs1 = h.recv_targeted(1, "round_start");

    REQUIRE(rs0.has_value());
    REQUIRE(rs1.has_value());
    CHECK(rs0->value("player_slot", -1) == 0);
    CHECK(rs1->value("player_slot", -1) == 1);
    CHECK(rs0->contains("round_end_at"));
    CHECK(rs0->contains("hand"));
    CHECK(rs0->contains("balance"));
}

// ─── G4: buy-in deducted from balance in round_start payload ──────────────────

TEST_CASE("G4: balance in round_start reflects buy-in deduction",
          "[game_session]") {
    run_migrations();
    Harness h;   // starting_balance=1000, pot_size=200, 2 players → buy_in=100
    h.connect_all();
    h.push(NetStartGame{});
    h.recv_type("round_starting");

    const auto rs = h.recv_targeted(0, "round_start");
    REQUIRE(rs.has_value());
    // balance after buy-in = 1000 - 100 = 900
    CHECK(rs->value("balance", -1) == 900);
}

// ─── G5: submit_order while RoundActive → order_ack ──────────────────────────

TEST_CASE("G5: submit_order while RoundActive produces order_ack",
          "[game_session]") {
    run_migrations();
    Harness h;
    h.advance_to_round_active();

    h.push(NetSubmit{0, "clubs", Side::Buy, 30});

    const auto ack = h.recv_targeted(0, "order_ack");
    REQUIRE(ack.has_value());
    CHECK(ack->value("suit",  "") == "clubs");
    CHECK(ack->value("side",  "") == "buy");
    CHECK(ack->value("price",  0) == 30);
    CHECK(ack->contains("order_id"));
}

// ─── G6: Crossing orders → trade + global wipe ────────────────────────────────

TEST_CASE("G6: crossing orders produce trade then null book_update wipe",
          "[game_session]") {
    run_migrations();
    Harness h;
    h.advance_to_round_active();

    // Resting bid at 50
    h.push(NetSubmit{0, "clubs", Side::Buy, 50});
    h.recv_targeted(0, "order_ack");  // consume ack

    // Crossing sell at 50
    h.push(NetSubmit{1, "clubs", Side::Sell, 50});
    h.recv_targeted(1, "order_ack");  // consume ack

    const auto trade = h.recv_type("trade");
    REQUIRE(trade.has_value());
    CHECK(trade->value("suit",  "") == "clubs");
    CHECK(trade->value("price",  0) == 50);

    // After trade, wipe sends null bid+ask
    const auto wipe = h.recv_type("book_update");
    REQUIRE(wipe.has_value());
    CHECK(wipe->at("best_bid").is_null());
    CHECK(wipe->at("best_ask").is_null());
}

// ─── G7: Round timer expiry → inter_round broadcast ───────────────────────────

TEST_CASE("G7: round timer expiry produces inter_round broadcast",
          "[game_session]") {
    run_migrations();
    ServerConfig cfg = make_cfg();
    cfg.game.round_duration_seconds = 0;  // expire immediately
    cfg.game.inter_round_seconds    = 60;

    Harness h(cfg);
    h.advance_to_round_active();

    const auto ir = h.recv_type("inter_round", 3000ms);
    REQUIRE(ir.has_value());
    CHECK(ir->value("round_number", -1) == 1);
    CHECK(ir->contains("goal_suit"));
    CHECK(ir->contains("results"));
    CHECK(ir->at("results").is_array());
    CHECK(ir->contains("next_round_at"));
}

// ─── G8: vote_to_end majority → game_ended ────────────────────────────────────

TEST_CASE("G8: majority vote_to_end produces game_ended",
          "[game_session]") {
    run_migrations();
    ServerConfig cfg = make_cfg();
    cfg.game.round_duration_seconds = 0;
    cfg.game.inter_round_seconds    = 60;

    Harness h(cfg);
    h.advance_to_round_active();

    h.recv_type("inter_round", 3000ms);  // wait for inter_round

    // (2+1)/2 = 1 — one vote is majority with 2 active players
    h.push(NetVoteToEnd{0});
    h.recv_type("vote_tally");

    const auto ended = h.recv_type("game_ended", 3000ms);
    REQUIRE(ended.has_value());
    CHECK(ended->contains("rounds"));
    CHECK(ended->contains("final_standings"));
    CHECK(ended->at("rounds").size() == 1);
    CHECK(ended->at("final_standings").size() == 2);
}

// ─── G9: disconnect during round → game_player_left broadcast ────────────────

TEST_CASE("G9: disconnect during RoundActive emits game_player_left",
          "[game_session]") {
    run_migrations();
    Harness h;
    h.advance_to_round_active();

    h.push(NetDisconnect{1});

    const auto left = h.recv_type("game_player_left");
    REQUIRE(left.has_value());
    CHECK(left->value("player_slot", -1) == 1);
    CHECK(left->value("username",    "") == "player1");
}

// ─── G10: submit_order before round is active → ROUND_NOT_ACTIVE ─────────────

TEST_CASE("G10: submit_order in Lobby phase returns ROUND_NOT_ACTIVE error",
          "[game_session]") {
    run_migrations();
    Harness h;
    h.connect_all();
    // Do NOT send start_game — session stays in Lobby phase.
    h.push(NetSubmit{0, "clubs", Side::Buy, 30});

    const auto err = h.recv_targeted(0, "error");
    REQUIRE(err.has_value());
    CHECK(err->value("code", "") == "ROUND_NOT_ACTIVE");
}
