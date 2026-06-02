#include <catch2/catch_test_macros.hpp>

#include "server/game_session.h"
#include "server/game_session_wire.h"
#include "server/session_queue.h"
#include "exchange/exchange_types.h"
#include "server/config.h"
#include "server/db.h"
#include "server/logger.h"
#include "server/eval/eval_module.h"
#include "server/eval/eval_types.h"

#include <readerwriterqueue.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace anjeer::server;
using namespace anjeer::engine;
using namespace anjeer::exchange;
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
                     const std::string& lobby_id   = "test-lobby",
                     std::vector<std::unique_ptr<eval::EvalModule>> eval_mods = {})
        : server_log("logs/gs_test_server.txt")
        , engine_log("logs/gs_test_engine.txt")
        , cfg(std::move(c))
    {
        GameSessionContext ctx{cfg, server_log, engine_log, rng, test_db()};
        ctx.eval_modules = std::move(eval_mods);
        session = std::make_unique<GameSession>(
            session_id, lobby_id, std::move(slots),
            std::move(ctx),
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
                    if constexpr (std::is_same_v<T, GameBroadcast>)  return e.json;
                    if constexpr (std::is_same_v<T, GameTargeted>)   return e.json;
                    if constexpr (std::is_same_v<T, GameBookUpdate>) return e.mbp1_json;
                    if constexpr (std::is_same_v<T, GameMboEvent>)   return e.json;
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

    // Drain outbound until a GameReconnectExpired for `slot` arrives.
    bool recv_reconnect_expired(int32_t slot,
                                std::chrono::milliseconds timeout = 3000ms) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            GameEvent ev;
            if (outbound.try_dequeue(ev)) {
                if (const auto* e = std::get_if<GameReconnectExpired>(&ev))
                    if (e->slot == slot) return true;
            } else {
                std::this_thread::sleep_for(10ms);
            }
        }
        return false;
    }

    // Drain outbound until a GameSpawnBot for `slot` arrives.
    std::optional<GameSpawnBot> recv_spawn_bot(int32_t slot,
                                               std::chrono::milliseconds timeout = 3000ms) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            GameEvent ev;
            if (outbound.try_dequeue(ev)) {
                if (const auto* e = std::get_if<GameSpawnBot>(&ev))
                    if (e->slot == slot) return *e;
            } else {
                std::this_thread::sleep_for(10ms);
            }
        }
        return std::nullopt;
    }

    std::optional<eval::EvalOutput> recv_eval_output(
            std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            GameEvent ev;
            if (outbound.try_dequeue(ev)) {
                if (auto* e = std::get_if<GameEvalOutput>(&ev))
                    return e->out;
            } else {
                std::this_thread::sleep_for(10ms);
            }
        }
        return std::nullopt;
    }

    // Drain outbound until a GameMboEvent whose JSON has the given type field arrives.
    std::optional<nlohmann::json> recv_mbo(
            const std::string& type,
            std::chrono::milliseconds timeout = 2000ms) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            GameEvent ev;
            if (outbound.try_dequeue(ev)) {
                if (const auto* m = std::get_if<GameMboEvent>(&ev)) {
                    auto j = nlohmann::json::parse(m->json);
                    if (j.value("type", "") == type) return j;
                }
            } else {
                std::this_thread::sleep_for(10ms);
            }
        }
        return std::nullopt;
    }

    // Drain outbound until a GameTargeted GameMboEvent (targeted by slot) arrives.
    std::optional<nlohmann::json> recv_targeted_mbo(
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

// ─── G8: owner end game → game_ended ─────────────────────────────────────────

TEST_CASE("G8: owner NetOwnerEndGame produces game_ended",
          "[game_session]") {
    run_migrations();
    ServerConfig cfg = make_cfg();
    cfg.game.round_duration_seconds = 0;
    cfg.game.inter_round_seconds    = 60;

    Harness h(cfg);
    h.advance_to_round_active();

    h.recv_type("inter_round", 3000ms);

    h.push(NetOwnerEndGame{});

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

// ─── G-T1: handle_player_disconnect logs and broadcasts player_left ───────────

// handle_player_disconnect enqueues NetReconnectDisconnect; game loop emits
// game_player_left to inform remaining players.
TEST_CASE("G-T1: handle_player_disconnect during RoundActive broadcasts game_player_left",
          "[game_session][reconnect]") {
    run_migrations();
    Harness h;
    h.advance_to_round_active();

    h.session->handle_player_disconnect(1);

    const auto left = h.recv_type("game_player_left");
    REQUIRE(left.has_value());
    CHECK(left->value("player_slot", -1) == 1);
    CHECK(left->value("username",    "") == "player1");
}

// ─── G-T2: cancel_orders_for_slot broadcasts book_update ─────────────────────

// Orders for disconnecting slot must be cancelled immediately so remaining
// players see an accurate book and can act on open prices.
TEST_CASE("G-T2: handle_player_disconnect cancels open orders and broadcasts book_update",
          "[game_session][reconnect]") {
    run_migrations();
    Harness h;
    h.advance_to_round_active();

    // Slot 0 places a bid; drain both the order_ack and the placement book_update
    // so the queue is clean before we trigger the disconnect.
    h.push(NetSubmit{0, "clubs", Side::Buy, 30});
    REQUIRE(h.recv_targeted(0, "order_ack").has_value());
    REQUIRE(h.recv_type("book_update").has_value());  // placement broadcast (best_bid=30)

    // Disconnect slot 0 — should cancel the order and broadcast book_update.
    h.session->handle_player_disconnect(0);

    // cancel_orders_for_slot emits book_update BEFORE game_player_left, so drain
    // book_update first to avoid recv_type("game_player_left") discarding it.
    const auto book_upd = h.recv_type("book_update");
    REQUIRE(book_upd.has_value());
    CHECK(book_upd->value("suit", "") == "clubs");
    CHECK(book_upd->at("best_bid").is_null());
}

// ─── G-T3: reconnect window expiry emits GameReconnectExpired + GameSpawnBot ──

// After reconnect_window_seconds elapses the slot must be marked expired and
// a bot spawned so the round continues with a full table.
TEST_CASE("G-T3: reconnect window expiry fires GameReconnectExpired and GameSpawnBot",
          "[game_session][reconnect]") {
    run_migrations();
    ServerConfig cfg = make_cfg();
    cfg.reconnect.reconnect_window_seconds = 1;  // short window for test speed
    Harness h(cfg);
    h.advance_to_round_active();

    h.session->handle_player_disconnect(1);
    // Drain the game_player_left so it doesn't block recv_reconnect_expired.
    h.recv_type("game_player_left");

    CHECK(h.recv_reconnect_expired(1, 3000ms));
    CHECK(h.recv_spawn_bot(1, 500ms).has_value());
}

// ─── G-T3b: last real player expiry ends game without spawning a bot ─────────
//
// Regression for: GameSpawnBot was enqueued unconditionally in
// check_reconnect_expirations before the real_player_count_ == 0 guard fired,
// causing GameSpawnBot + GameDone to land on the outbound queue in the same
// tick.  WsServer processed the spawn first, registered a BotAdapter holding
// a reference into the session, then GameDone tore the session down —
// use-after-free / null-deref on the uWS event-loop thread, silent crash.
//
// Fixed by: moving the real_player_count_ == 0 early-return above the
// GameSpawnBot enqueue in check_reconnect_expirations.
TEST_CASE("G-T3b: last real player reconnect expiry emits GameDone, not GameSpawnBot",
          "[game_session][reconnect]") {
    run_migrations();
    ServerConfig cfg = make_cfg();
    cfg.game.player_count                  = 1;
    cfg.lobby.min_players                  = 1;
    cfg.reconnect.reconnect_window_seconds = 0;
    Harness h(cfg, make_slots(1));

    h.push(NetConnect{0, 1, "player0"});
    h.push(NetStartGame{});
    REQUIRE(h.recv_type("round_starting").has_value());
    REQUIRE(h.recv_targeted(0, "round_start").has_value());

    h.session->handle_player_disconnect(0);
    h.recv_type("game_player_left");

    REQUIRE(h.recv_reconnect_expired(0, 3000ms));

    // Session must end cleanly. recv_done runs before recv_spawn_bot so
    // GameDone is not accidentally consumed by the spawn-bot drain loop.
    CHECK(h.recv_done(2000ms));
    // No bot spawn — there is no game left to spawn into.
    CHECK_FALSE(h.recv_spawn_bot(0, 300ms).has_value());
}

// ─── G-T3c: NetPermanentLeave bypasses reconnect window ──────────────────────
//
// Regression for: onLeave in Game.tsx previously called navigate() without
// sending leave_lobby first.  The WS closed on component unmount, triggering
// onDisconnect → handle_player_disconnect → reconnect window.  The game kept
// running with bots for reconnect_window_seconds even though the player
// intentionally left.
//
// Fixed by: Game.tsx sends leave_lobby before navigate().  WsServer's
// handle_leave_lobby enqueues NetPermanentLeave and removes the WS handle from
// ws_to_slot_, so the subsequent onDisconnect (from component unmount) finds no
// slot entry and never starts the reconnect window.
//
// This test verifies the server-side contract: NetPermanentLeave must produce
// GameDone immediately — well before reconnect_window_seconds would elapse.
// reconnect_window_seconds is set to 5 s so a regression (where permanent leave
// mistakenly starts the reconnect path) causes recv_done to time out.
TEST_CASE("G-T3c: NetPermanentLeave ends game immediately, not after reconnect window",
          "[game_session][leave]") {
    run_migrations();
    ServerConfig cfg = make_cfg();
    cfg.game.player_count                  = 1;
    cfg.lobby.min_players                  = 1;
    cfg.reconnect.reconnect_window_seconds = 5;  // long window — GameDone must beat it
    Harness h(cfg, make_slots(1));

    h.push(NetConnect{0, 1, "player0"});
    h.push(NetStartGame{});
    REQUIRE(h.recv_type("round_starting").has_value());
    REQUIRE(h.recv_targeted(0, "round_start").has_value());

    h.push(NetPermanentLeave{0});
    h.recv_type("game_player_left");

    // GameDone must arrive within 2 s — far below the 5 s reconnect window.
    CHECK(h.recv_done(2000ms));
    // No reconnect window should ever be started for a permanent leave.
    CHECK_FALSE(h.recv_reconnect_expired(0, 200ms));
}

// ─── G-T4: handle_player_reattach sends game_state_snapshot ──────────────────

// On reattach the returning player must receive a full state snapshot so their
// client can repopulate the board without a page reload.
TEST_CASE("G-T4: handle_player_reattach emits game_state_snapshot to the slot",
          "[game_session][reconnect]") {
    run_migrations();
    ServerConfig cfg = make_cfg();
    cfg.reconnect.reconnect_window_seconds = 10;
    Harness h(cfg);
    h.advance_to_round_active();

    h.session->handle_player_disconnect(1);
    h.recv_type("game_player_left");  // drain

    // Reattach within window with a dummy token.
    h.session->handle_player_reattach(1, "rtk_test_token", 9999999);

    const auto snap = h.recv_targeted(1, "game_state_snapshot", 2000ms);
    REQUIRE(snap.has_value());
    CHECK(snap->contains("hand"));
    CHECK(snap->contains("order_books"));
    CHECK(snap->contains("all_balances"));
    CHECK(snap->value("player_slot", -1) == 1);
    CHECK(snap->value("reconnect_token", "") == "rtk_test_token");
}

// ─── G-T5: admit_from_queue fills an inactive slot ───────────────────────────

// Queued players admitted at round boundaries must be visible to the session
// as active real-player slots for the next round.
TEST_CASE("G-T5: admit_from_queue activates a previously inactive slot",
          "[game_session][reconnect]") {
    run_migrations();
    ServerConfig cfg = make_cfg();
    cfg.reconnect.reconnect_window_seconds = 1;
    Harness h(cfg);
    h.advance_to_round_active();

    // Expire slot 1 so it becomes inactive.
    h.session->handle_player_disconnect(1);
    h.recv_type("game_player_left");
    REQUIRE(h.recv_reconnect_expired(1, 3000ms));
    h.recv_spawn_bot(1, 500ms);  // drain

    // Admit a new player via NetAdmitQueue — game-loop processes it on its thread.
    h.push(NetAdmitQueue{{{1, 99, "newcomer"}}});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_FALSE(h.session->is_done());
}

// ─── G-T6: hand_for_slot returns zero hand when no round active ───────────────

TEST_CASE("G-T6: hand_for_slot returns zero hand before round starts",
          "[game_session][reconnect]") {
    run_migrations();
    Harness h;
    h.connect_all();
    // Lobby phase — no game_state_
    const auto hand = h.session->hand_for_slot(0);
    for (int i = 0; i < 4; ++i) CHECK(hand.suit_counts[i] == 0);
}

// ─── Eval wiring tests ────────────────────────────────────────────────────────

namespace {

struct CountingModule : anjeer::server::eval::EvalModule {
    std::atomic<int> round_starts{0};
    std::atomic<int> trades{0};
    std::atomic<int> book_updates{0};
    std::atomic<int> round_ends{0};

    void on_round_start(const anjeer::engine::GameStateSnapshot&) override { ++round_starts; }
    void on_trade_event(const anjeer::server::eval::EvalTradeEvent&) override { ++trades; }
    void on_book_update(const anjeer::server::eval::EvalBookUpdate&) override { ++book_updates; }
    void on_round_end  (const anjeer::engine::GameStateSnapshot&) override { ++round_ends; }

    void wait_for(std::atomic<int>& counter, int n,
                  std::chrono::milliseconds timeout = std::chrono::milliseconds(1000)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (counter.load() < n && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
};

// Emits one private (slot 0) and one public (-1) EvalOutput on round start.
struct EmittingModule : anjeer::server::eval::EvalModule {
    void on_round_start(const anjeer::engine::GameStateSnapshot&) override {
        emit({anjeer::server::eval::EvalOutput::Type::PosteriorUpdate, 0,
              nlohmann::json{{"type", "eval.posterior_update"}}});
        emit({anjeer::server::eval::EvalOutput::Type::AccumulationSignal, -1,
              nlohmann::json{{"type", "eval.accumulation_signal"}}});
    }
    void on_trade_event(const anjeer::server::eval::EvalTradeEvent&) override {}
    void on_book_update(const anjeer::server::eval::EvalBookUpdate&) override {}
    void on_round_end  (const anjeer::engine::GameStateSnapshot&) override {}
};

} // namespace

// G-E1: push_round_start fired exactly once when a round begins.
TEST_CASE("G-E1: eval push_round_start fires once per round start", "[game_session][eval]") {
    run_migrations();
    auto* mod = new CountingModule;
    std::vector<std::unique_ptr<anjeer::server::eval::EvalModule>> mods;
    mods.push_back(std::unique_ptr<anjeer::server::eval::EvalModule>(mod));

    Harness h(make_cfg(), make_slots(2), "eval-session-1", "eval-lobby-1", std::move(mods));
    h.advance_to_round_active();

    mod->wait_for(mod->round_starts, 1);
    CHECK(mod->round_starts.load() == 1);
}

// G-E2: push_round_end fired once when the round ends.
TEST_CASE("G-E2: eval push_round_end fires once per round end", "[game_session][eval]") {
    run_migrations();
    ServerConfig cfg = make_cfg();
    cfg.game.round_duration_seconds = 1;
    cfg.game.inter_round_seconds    = 60;

    auto* mod = new CountingModule;
    std::vector<std::unique_ptr<anjeer::server::eval::EvalModule>> mods;
    mods.push_back(std::unique_ptr<anjeer::server::eval::EvalModule>(mod));

    Harness h(cfg, make_slots(2), "eval-session-2", "eval-lobby-2", std::move(mods));
    h.advance_to_round_active();

    // Wait for the round to time out (1s) and round_end to fire.
    mod->wait_for(mod->round_ends, 1, 3000ms);
    CHECK(mod->round_ends.load() == 1);
    CHECK(mod->round_starts.load() == 1);
}

// G-E3: push_trade fired after each match in the book.
TEST_CASE("G-E3: eval push_trade fires for each matched trade", "[game_session][eval]") {
    run_migrations();
    ServerConfig cfg = make_cfg();
    cfg.order_book.active_suits = {"clubs"};

    auto* mod = new CountingModule;
    std::vector<std::unique_ptr<anjeer::server::eval::EvalModule>> mods;
    mods.push_back(std::unique_ptr<anjeer::server::eval::EvalModule>(mod));

    Harness h(cfg, make_slots(2), "eval-session-3", "eval-lobby-3", std::move(mods));
    h.advance_to_round_active();

    // Slot 0 sells at 50, slot 1 buys at 50 → immediate match.
    h.push(NetSubmit{0, "clubs", anjeer::engine::Side::Sell, 50});
    h.push(NetSubmit{1, "clubs", anjeer::engine::Side::Buy,  50});

    mod->wait_for(mod->trades, 1, 1000ms);
    CHECK(mod->trades.load() >= 1);
}

// G-E4: EvalModule emits private + public outputs; verify GameEvalOutput target_slots.
TEST_CASE("G-E4: eval output routed to correct target_slot in outbound queue",
          "[game_session][eval]") {
    run_migrations();
    auto* mod = new EmittingModule;
    std::vector<std::unique_ptr<anjeer::server::eval::EvalModule>> mods;
    mods.push_back(std::unique_ptr<anjeer::server::eval::EvalModule>(mod));

    Harness h(make_cfg(), make_slots(2), "eval-session-4", "eval-lobby-4", std::move(mods));
    h.advance_to_round_active();

    // EmittingModule emits on on_round_start: slot-0 private + broadcast (-1).
    // The outputs flow: output_cb → eval_out_pending_ → drain_eval_output() → outbound.
    auto out1 = h.recv_eval_output();
    auto out2 = h.recv_eval_output();

    REQUIRE(out1.has_value());
    REQUIRE(out2.has_value());

    std::unordered_map<int, anjeer::server::eval::EvalOutput> by_slot;
    by_slot[out1->target_slot] = *out1;
    by_slot[out2->target_slot] = *out2;

    REQUIRE(by_slot.count(0));
    CHECK(by_slot.at(0).payload.value("type", "") == "eval.posterior_update");

    REQUIRE(by_slot.count(-1));
    CHECK(by_slot.at(-1).payload.value("type", "") == "eval.accumulation_signal");
}

// ── T17 — MBO on-connect snapshot ────────────────────────────────────────────
// NetSendFeedSnapshot{slot, "mbo"} → GameTargeted per instrument with
// type="order_book_snapshot".
TEST_CASE("T17: MBO on-connect snapshot emits order_book_snapshot per instrument",
          "[game_session][mbo][T17]") {
    run_migrations();
    auto cfg = make_cfg();
    cfg.order_book.active_suits = {"clubs", "diamonds", "hearts", "spades"};
    Harness h(cfg);
    h.connect_all();
    h.push(NetStartGame{});
    REQUIRE(h.recv_type("round_starting").has_value());
    REQUIRE(h.recv_type("round_start").has_value());

    h.push(NetSendFeedSnapshot{0, "mbo"});

    int snapshots = 0;
    const auto deadline = std::chrono::steady_clock::now() + 2000ms;
    while (std::chrono::steady_clock::now() < deadline && snapshots < 4) {
        GameEvent ev;
        if (h.outbound.try_dequeue(ev)) {
            if (const auto* t = std::get_if<GameTargeted>(&ev)) {
                if (t->slot == 0) {
                    auto j = nlohmann::json::parse(t->json);
                    if (j.value("type", "") == "order_book_snapshot") {
                        CHECK(j.contains("v"));
                        CHECK(j.contains("seq"));
                        CHECK(j.contains("suit"));
                        CHECK(j.contains("bids"));
                        CHECK(j.contains("asks"));
                        ++snapshots;
                    }
                }
            }
        } else {
            std::this_thread::sleep_for(10ms);
        }
    }
    CHECK(snapshots == 4);
}

// ── T18 — MBO incremental events ─────────────────────────────────────────────
// submit → GameMboEvent order_added; cancel → GameMboEvent order_cancelled;
// crossing submit → GameMboEvent order_executed.
TEST_CASE("T18: MBO incremental: order_added on submit, order_cancelled on cancel, order_executed on match",
          "[game_session][mbo][T18]") {
    run_migrations();
    auto cfg = make_cfg();
    cfg.order_book.active_suits = {"clubs"};
    Harness h(cfg);
    h.connect_all();
    h.push(NetStartGame{});
    REQUIRE(h.recv_type("round_starting").has_value());
    REQUIRE(h.recv_type("round_start").has_value());

    // Submit a resting bid → expect order_added
    h.push(NetSubmit{0, "clubs", Side::Buy, 50});
    auto added = h.recv_mbo("order_added");
    REQUIRE(added.has_value());
    CHECK(added->value("suit", "") == "clubs");
    CHECK(added->value("side", "") == "buy");
    CHECK(added->value("price", 0) == 50);
    CHECK(added->contains("order_id"));
    CHECK(added->contains("seq"));
    CHECK(added->value("v", 0) == 1);

    const int64_t order_id = (*added)["order_id"].get<int64_t>();

    // Cancel that order → expect order_cancelled
    h.push(NetCancel{0, order_id});
    auto cancelled = h.recv_mbo("order_cancelled");
    REQUIRE(cancelled.has_value());
    CHECK(cancelled->value("order_id", int64_t{0}) == order_id);
    CHECK(cancelled->value("suit", "") == "clubs");
    CHECK(cancelled->contains("seq"));

    // Place a new bid then a crossing ask → expect order_executed
    h.push(NetSubmit{0, "clubs", Side::Buy, 60});
    REQUIRE(h.recv_mbo("order_added").has_value());
    h.push(NetSubmit{1, "clubs", Side::Sell, 60});
    auto executed = h.recv_mbo("order_executed");
    REQUIRE(executed.has_value());
    CHECK(executed->value("suit", "") == "clubs");
    CHECK(executed->value("price", 0) == 60);
    CHECK(executed->contains("aggressor_side"));
    CHECK(executed->contains("buyer_slot"));
    CHECK(executed->contains("seller_slot"));
    CHECK(executed->contains("seq"));
    CHECK(executed->value("v", 0) == 1);
}
