#include "server/ws_server.h"
#include "server/game_session.h"
#include "server/logger.h"

#include <nlohmann/json.hpp>

#include <random>
#include <string>
#include <stdexcept>

namespace anjeer::server {

WsServer::WsServer(const ServerConfig& cfg, LobbyGateway& lobby_gateway)
    : cfg_(cfg)
    , lobby_gateway_(lobby_gateway)
{}

void WsServer::run() {
    Logger server_log("logs/server_logs.txt");
    Logger engine_log("logs/engine_logs.txt");
    Logger frontend_log("logs/frontend_logs.txt");

    server_log.info("startup", "server starting — config loaded");

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

    // Shared across all deals; safe — all engine calls run on the event-loop thread.
    std::mt19937 rng{std::random_device{}()};

    uWS::App   app;
    uWS::Loop* loop = uWS::Loop::get();

    GameSession session(std::move(books_arr), active_arr, cfg_,
                        server_log, engine_log, loop, rng);

    app.ws<PerSocketData>("/ws", {
        .idleTimeout            = static_cast<unsigned short>(cfg_.ping_timeout_ms / 1000),
        .sendPingsAutomatically = true,

        .open = [&](WsHandle ws) {
            session.on_connect(ws);
        },

        .message = [&](WsHandle ws, std::string_view msg, uWS::OpCode op) {
            try {
                const auto j    = nlohmann::json::parse(msg);
                const auto type = j.value("type", "");
                if (type == "subscribe_lobby") {
                    lobby_gateway_.handle_subscribe(ws, j.value("lobby_id", ""), loop, server_log);
                    return;
                }
                if (type == "unsubscribe_lobby") {
                    lobby_gateway_.handle_unsubscribe(ws);
                    return;
                }
            } catch (const nlohmann::json::exception&) {
                // JSON parse failure — fall through to session for proper error
            }
            session.on_message(ws, msg, op);
        },

        .close = [&](WsHandle ws, int code, std::string_view reason) {
            lobby_gateway_.cleanup(ws);
            session.on_close(ws, code, reason);
        },
    });

    // POST /api/log: body arrives in chunks; CORS gated to cors_origin.
    app.post("/api/log", [&](auto* res, auto* req) {
        const std::string allowed_origin = cfg_.cors_origin;
        std::string_view origin = req->getHeader("origin");
        if (origin != allowed_origin) {
            res->writeStatus("403 Forbidden")->end("");
            return;
        }

        auto body_buf = std::make_shared<std::string>();
        body_buf->reserve(4096);

        res->onData([res, body_buf, allowed_origin, &frontend_log](std::string_view chunk, bool last) {
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

            res->cork([res, allowed_origin]() {
                res->writeHeader("Content-Type", "text/plain")
                   ->writeHeader("Access-Control-Allow-Origin", allowed_origin)
                   ->end("ok");
            });
        });

        res->onAborted([body_buf]() {});
    });

    app.options("/api/log", [&](auto* res, auto* req) {
        std::string_view origin = req->getHeader("origin");
        if (origin != cfg_.cors_origin) {
            res->writeStatus("403 Forbidden")->end("");
            return;
        }
        res->writeHeader("Access-Control-Allow-Origin", cfg_.cors_origin)
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

    session.shutdown();  // join timer threads before stack locals are destroyed
    server_log.info("shutdown", "server stopped");
}

} // namespace anjeer::server
