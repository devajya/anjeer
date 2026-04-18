#include "server/ws_server.h"
#include "server/game_session.h"
#include "server/logger.h"

// AGENT-CTX: nlohmann/json is used here only for the /api/log endpoint body
// parsing and the CORS responses. All game-message serialisation lives in
// game_session.cpp. If /api/log moves to the Crow HTTP server (Slice 5+),
// this include can be removed.
#include <nlohmann/json.hpp>

#include <random>
#include <string>
#include <stdexcept>

namespace anjeer::server {

WsServer::WsServer(const ServerConfig& cfg) : cfg_(cfg) {}

void WsServer::run() {
    // AGENT-CTX: Log files live in logs/ relative to the process working
    // directory. `make dev` runs from the project root so logs appear at
    // <project_root>/logs/. Logger creates the directory if absent.
    Logger server_log("logs/server_logs.txt");
    Logger engine_log("logs/engine_logs.txt");
    Logger frontend_log("logs/frontend_logs.txt");

    server_log.info("startup", "server starting — config loaded");

    // Build all 4 books with identical price config; each gets its own suit name.
    auto make_book = [&](engine::Suit s) {
        return engine::OrderBook{engine::OrderBook::Config{
            cfg_.order_book.min_price,
            cfg_.order_book.max_price,
            cfg_.order_book.nudge_initial_buy_price,
            cfg_.order_book.nudge_initial_sell_price,
            std::string(engine::suit_name(s)),
        }};
    };
    std::array<engine::OrderBook, 4> books_arr{
        make_book(engine::Suit::Clubs),
        make_book(engine::Suit::Diamonds),
        make_book(engine::Suit::Hearts),
        make_book(engine::Suit::Spades),
    };

    // Validate active_suits from config; fail loud on any unrecognized name.
    std::array<bool, 4> active_arr{};
    for (const auto& suit_str : cfg_.order_book.active_suits) {
        const auto s = engine::suit_from_string(suit_str);
        if (!s) throw std::runtime_error(
            "active_suits contains unrecognized suit: '" + suit_str + "'");
        active_arr[engine::suit_index(*s)] = true;
        server_log.info("startup", "registered suit: " + suit_str);
    }

    // AGENT-CTX: Seeded once per process from OS entropy. Single instance
    // shared across all deals in this process lifetime (safe — all engine
    // calls are on the event-loop thread, no concurrent access).
    std::mt19937 rng{std::random_device{}()};

    uWS::App   app;
    uWS::Loop* loop = uWS::Loop::get();

    // AGENT-CTX: GameSession owns all game state and its own timer threads.
    // WsServer::run() is the factory that creates it and holds it for the
    // lifetime of the event loop. shutdown() is called after app.run() returns
    // (on SIGINT) to join threads before stack locals are destroyed.
    GameSession session(std::move(books_arr), active_arr, cfg_,
                        server_log, engine_log, loop, rng);

    // ── WebSocket handler ─────────────────────────────────────────────────
    // AGENT-CTX: All game logic is delegated to session. These lambdas are
    // intentionally thin — they are the seam LobbyManager will replace in
    // Slice 6 with lobby-scoped session lookups.
    app.ws<PerSocketData>("/ws", {
        .idleTimeout            = static_cast<unsigned short>(cfg_.ping_timeout_ms / 1000),
        .sendPingsAutomatically = true,

        .open = [&](WsHandle ws) {
            session.on_connect(ws);
        },

        .message = [&](WsHandle ws, std::string_view msg, uWS::OpCode op) {
            session.on_message(ws, msg, op);
        },

        .close = [&](WsHandle ws, int code, std::string_view reason) {
            session.on_close(ws, code, reason);
        },
    });

    // ── HTTP: POST /api/log — accepts frontend log entries ────────────────
    // AGENT-CTX: Frontend logger POSTs JSON arrays of log entries here.
    // uWS HTTP bodies arrive in chunks; accumulated in a per-request string.
    // This endpoint is dev-only — no auth, no rate limiting, no size cap.
    // TODO(slice5): move this to the Crow HTTP server so it can be gated
    // behind the same middleware as other /api/* routes.
    app.post("/api/log", [&](auto* res, auto* /*req*/) {
        auto body_buf = std::make_shared<std::string>();
        body_buf->reserve(4096);

        res->onData([res, body_buf, &frontend_log](std::string_view chunk, bool last) {
            constexpr std::size_t kMaxBody = 1 * 1024 * 1024;
            if (body_buf->size() + chunk.size() > kMaxBody) {
                res->close();
                return;
            }
            *body_buf += chunk;
            if (!last) return;

            try {
                const auto arr = nlohmann::json::parse(*body_buf);
                if (arr.is_array()) {
                    for (const auto& entry : arr) {
                        const std::string level   = entry.value("level",     "INFO");
                        const std::string comp    = entry.value("component", "frontend");
                        const std::string message = entry.value("message",   "");
                        const std::string data    = entry.contains("data") && !entry["data"].is_null()
                            ? " data=" + entry["data"].dump() : "";
                        if      (level == "ERROR") frontend_log.error(comp, message + data);
                        else if (level == "WARN")  frontend_log.warn (comp, message + data);
                        else if (level == "DEBUG") frontend_log.debug(comp, message + data);
                        else                       frontend_log.info  (comp, message + data);
                    }
                }
            } catch (const std::exception& ex) {
                frontend_log.warn("/api/log", std::string("parse error: ") + ex.what());
            }

            res->cork([res]() {
                res->writeHeader("Content-Type", "text/plain")
                   ->writeHeader("Access-Control-Allow-Origin", "*")
                   ->end("ok");
            });
        });

        res->onAborted([body_buf]() {});
    });

    // ── OPTIONS /api/log — CORS preflight ─────────────────────────────────
    app.options("/api/log", [](auto* res, auto* /*req*/) {
        res->writeHeader("Access-Control-Allow-Origin", "*")
           ->writeHeader("Access-Control-Allow-Methods", "POST, OPTIONS")
           ->writeHeader("Access-Control-Allow-Headers", "Content-Type")
           ->end("");
    });

    bool listen_ok = false;
    app.listen(cfg_.host, cfg_.port, [&](auto* token) {
        if (token) {
            listen_ok = true;
            server_log.info("startup",
                            "listening on " + cfg_.host + ":" + std::to_string(cfg_.port));
            std::cout << "[server] listening on "
                      << cfg_.host << ":" << cfg_.port << '\n';
        } else {
            server_log.error("startup",
                             "failed to listen on port " + std::to_string(cfg_.port));
            std::cerr << "[server] failed to listen on port " << cfg_.port << '\n';
        }
    });
    if (!listen_ok) {
        throw std::runtime_error("failed to listen on port " + std::to_string(cfg_.port));
    }

    app.run();

    // AGENT-CTX: shutdown() joins timer threads before stack locals
    // (server_log, engine_log, rng) are destroyed. Order matters.
    session.shutdown();
    server_log.info("shutdown", "server stopped");
}

} // namespace anjeer::server
