#include "server/ws_server.h"
#include "server/crypto_util.h"
#include "server/game_session_wire.h"

#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include <algorithm>
#include <string>
#include <stdexcept>
#include <vector>
#include <variant>

// AGENT-CTX: us_create_timer / us_timer_set are the libuSockets C timer API.
// They are the correct way to schedule periodic work on the uWS event loop
// without blocking. The 16ms period matches a 60fps drain cadence — tight
// enough that outbound queue latency is imperceptible, loose enough to avoid
// burning CPU. us_loop_t* is obtained by casting uWS::Loop::get().
#include <libusockets.h>

namespace anjeer::server {

// ─── Cookie parsing helper ─────────────────────────────────────────────────
static std::string parse_cookie_value(std::string_view header, std::string_view name)
{
    while (!header.empty()) {
        while (!header.empty() && header.front() == ' ') header.remove_prefix(1);
        const auto semi = header.find(';');
        const auto tok  = semi != std::string_view::npos ? header.substr(0, semi) : header;
        header          = semi != std::string_view::npos ? header.substr(semi + 1)
                                                         : std::string_view{};
        const auto eq = tok.find('=');
        if (eq == std::string_view::npos) continue;
        if (tok.substr(0, eq) == name) {
            auto val = tok.substr(eq + 1);
            // trim leading/trailing spaces
            while (!val.empty() && val.front() == ' ') val.remove_prefix(1);
            while (!val.empty() && val.back()  == ' ') val.remove_suffix(1);
            return std::string(val);
        }
    }
    return {};
}

// ─── Open-role resolution ─────────────────────────────────────────────────
enum class OpenRole { Player, SpectatorFallthrough, Reject };

static OpenRole resolve_open_role(AuthType auth, LobbyMode mode)
{
    if (auth == AuthType::ApiKey && mode == LobbyMode::UI)  return OpenRole::Reject;
    if (auth == AuthType::JWT    && mode == LobbyMode::API) return OpenRole::SpectatorFallthrough;
    return OpenRole::Player;
}

// ─── Constructor ──────────────────────────────────────────────────────────
WsServer::WsServer(const ServerConfig& cfg, WsServerDeps deps)
    : cfg_          (cfg)
    , lobby_gateway_(deps.lobby_gateway)
    , db_pool_      (deps.db_pool)
    , lobby_repo_   (deps.lobby_repo)
    , auth_service_ (deps.auth_service)
    , api_key_repo_ (deps.api_key_repo)
    , event_bus_    (deps.event_bus)
    , rate_limiter_ (RateLimitConfig{
          cfg.rate_limit.capacity,
          cfg.rate_limit.refill_rate,
          cfg.rate_limit.suspend_threshold,
          cfg.rate_limit.suspend_seconds})
    , server_log_   ("logs/server_logs.txt")
    , engine_log_   ("logs/engine_logs.txt")
    , frontend_log_ ("logs/frontend_logs.txt")
    , rng_          (std::random_device{}())
{}

// ─── run() — blocks until SIGINT ──────────────────────────────────────────
void WsServer::run() {
    server_log_.info("startup", "server starting — config loaded");

    uWS::App   app;
    uWS::Loop* loop = uWS::Loop::get();

    // AGENT-CTX: Subscribe to "game:start" on the LocalEventBus. The handler
    // fires synchronously on the Crow thread (HttpServer publishes there after
    // a successful lobby start). loop->defer() marshals the actual session
    // creation back to the uWS event-loop thread so active_sessions_ is only
    // ever mutated from one thread. Without defer() this would be a data race.
    game_start_sub_id_ = event_bus_.subscribe("game:start",
        [this, loop](const std::string& payload) {
            try {
                const auto j        = nlohmann::json::parse(payload);
                std::string lobby_id = j.value("lobby_id", "");
                if (lobby_id.empty()) return;
                loop->defer([this, lobby_id]() {
                    create_session(lobby_id);
                });
            } catch (...) {
                server_log_.error("game:start", "failed to parse game:start event");
            }
        });

    // ── WebSocket endpoint ─────────────────────────────────────────────────
    app.ws<PerSocketData>("/ws", {
        .idleTimeout            = static_cast<unsigned short>(cfg_.ping_timeout_ms / 1000),
        .sendPingsAutomatically = true,

        // AGENT-CTX: .upgrade fires before .open and has access to the HTTP
        // request headers (including cookies). This is the only place we can
        // read auth credentials because uWS does not expose request headers
        // in .open. Auth priority: (1) Authorization: Bearer <api_key> header,
        // (2) ?api_key= query param (insecure fallback, logged as warning),
        // (3) JWT access_token cookie. Rejected connections (invalid key,
        // suspended player) are still upgraded so they receive a typed JSON
        // error before the server closes the frame.
        // ARCHITECTURE-NOTE: find_valid_by_hash() is a synchronous DB call on
        // the uWS event-loop thread (~1 ms on local HW). Accepted single-node
        // risk for Slice 9; flagged for async migration in Slice 16.
        .upgrade = [this, loop](uWS::HttpResponse<false>* res,
                                uWS::HttpRequest*          req,
                                us_socket_context_t*       ctx) {
            int64_t    player_id   = -1;
            AuthType   auth_type   = AuthType::JWT;
            std::optional<WsErrorCode> pending_close;

            std::string_view auth_hdr = req->getHeader("authorization");
            std::string_view api_key_qp = req->getQuery("api_key");

            auto try_api_key = [&](std::string_view raw_key) {
                try {
                    auto handle = db_pool_.acquire();
                    pqxx::work txn(handle.get());
                    auto maybe = api_key_repo_.find_valid_by_hash(txn, sha256_hex(raw_key));
                    if (maybe) {
                        player_id = maybe->player_id;
                        auth_type = AuthType::ApiKey;
                    } else {
                        pending_close = WsErrorCode::ApiKeyInvalid;
                    }
                } catch (...) {
                    pending_close = WsErrorCode::ApiKeyInvalid;
                }
            };

            // --- Bearer header (preferred) ---
            if (auth_hdr.size() > 7 && auth_hdr.substr(0, 7) == "Bearer ") {
                try_api_key(auth_hdr.substr(7));
            }
            // --- Query-param fallback (insecure) ---
            else if (!api_key_qp.empty()) {
                server_log_.warn("upgrade",
                    "API key supplied via query param — use Authorization: Bearer in production");
                try_api_key(api_key_qp);
            }
            // --- JWT cookie ---
            else {
                std::string_view cookie_hdr = req->getHeader("cookie");
                if (!cookie_hdr.empty()) {
                    std::string token = parse_cookie_value(cookie_hdr, "access_token");
                    if (!token.empty()) {
                        auto opt = auth_service_.validate_access_token(token);
                        if (opt) player_id = *opt;
                    }
                }
            }

            // Reject suspended players before they re-enter
            if (player_id >= 0 && !pending_close &&
                rate_limiter_.is_suspended(static_cast<int32_t>(player_id))) {
                pending_close = WsErrorCode::RateLimitExceeded;
            }

            // AGENT-CTX: Username is not in the JWT (the token only carries player_id
            // as subject). We fetch it here in .upgrade so handle_leave_lobby can
            // build player_left broadcasts without a per-event DB round-trip. Skip
            // the query for connections we are about to reject.
            std::string username;
            if (player_id >= 0 && !pending_close) {
                try {
                    auto handle = db_pool_.acquire();
                    pqxx::work txn(handle.get());
                    auto rows = txn.exec_params(
                        "SELECT username FROM players WHERE id = $1", player_id);
                    if (!rows.empty()) username = rows[0][0].as<std::string>();
                } catch (...) {}
            }

            PerSocketData psd;
            psd.player_id    = player_id;
            psd.auth_type    = auth_type;
            psd.pending_close = pending_close;
            psd.username     = username;

            res->template upgrade<PerSocketData>(
                std::move(psd),
                req->getHeader("sec-websocket-key"),
                req->getHeader("sec-websocket-protocol"),
                req->getHeader("sec-websocket-extensions"),
                ctx);
        },

        .open = [this, loop](WsHandle ws) {
            auto* data = ws->getUserData();

            // Auth rejection or rate-limit suspension set in .upgrade: send
            // a typed error then close before the connection is used.
            if (data->pending_close) {
                ws->send(nlohmann::json{
                    {"type",    "error"},
                    {"code",    serialise::error_code_str(*data->pending_close)},
                    {"message", "connection rejected"}
                }.dump(), uWS::OpCode::TEXT);
                loop->defer([ws]() { ws->end(1008, "rejected"); });
                return;
            }

            const int64_t player_id = data->player_id;

            // Check if this player belongs to an active game session
            auto lobby_it = player_to_lobby_.find(player_id);
            if (player_id >= 0 && lobby_it != player_to_lobby_.end()) {
                const std::string& lobby_id = lobby_it->second;
                auto session_it = active_sessions_.find(lobby_id);
                if (session_it != active_sessions_.end()) {
                    auto& as = session_it->second;
                    // Find the slot for this player
                    int32_t slot = -1;
                    for (int i = 0; i < static_cast<int>(as.slots_.size()); ++i) {
                        if (as.slots_[i].player_id == player_id) { slot = i; break; }
                    }
                    if (slot >= 0) {
                        switch (resolve_open_role(data->auth_type, as.lobby_mode_)) {
                        case OpenRole::Reject:
                            ws->send(nlohmann::json{
                                {"type",    "error"},
                                {"code",    serialise::error_code_str(WsErrorCode::LobbyModeMismatch)},
                                {"message", "auth type does not match lobby mode"}
                            }.dump(), uWS::OpCode::TEXT);
                            server_log_.warn("open",
                                "mode mismatch for player " + std::to_string(player_id) +
                                " in lobby " + lobby_id);
                            loop->defer([ws]() { ws->end(1008, "lobby mode mismatch"); });
                            return;
                        case OpenRole::SpectatorFallthrough:
                            break;
                        case OpenRole::Player: {
                            // Handle reconnect: evict stale handle for this slot if present
                            auto old_it = as.slot_to_ws_.find(slot);
                            if (old_it != as.slot_to_ws_.end()) {
                                as.ws_to_slot_.erase(old_it->second);
                            }
                            as.slot_to_ws_[slot] = ws;
                            as.ws_to_slot_[ws]   = slot;
                            data->lobby_id    = lobby_id;
                            data->player_slot = slot;
                            as.inbound->enqueue(NetConnect{slot, player_id,
                                                           as.slots_[slot].username});
                            ws->send(nlohmann::json{
                                {"type","player_hello"}, {"player_id", player_id}
                            }.dump(), uWS::OpCode::TEXT);
                            server_log_.info("open", "player " + std::to_string(player_id) +
                                " connected as slot " + std::to_string(slot) +
                                " in lobby " + lobby_id);
                            return;
                        }
                        } // switch
                    }
                }
            }

            // Lobby-only socket (no active session or unauthenticated)
            ws->send(nlohmann::json{
                {"type","player_hello"}, {"player_id", player_id}
            }.dump(), uWS::OpCode::TEXT);
            server_log_.info("open", "lobby socket connected player_id=" +
                             std::to_string(player_id));
        },

        .message = [this, loop](WsHandle ws, std::string_view msg, uWS::OpCode op) {
            if (op != uWS::OpCode::TEXT) return;
            auto* data = ws->getUserData();

            // Rate limiting — skip for unauthenticated connections (player_id == -1)
            if (data->player_id >= 0) {
                auto rl = rate_limiter_.check(static_cast<int32_t>(data->player_id));
                if (rl == RateLimitResult::Warn) {
                    serialise::error(ws, WsErrorCode::RateLimitWarning,
                                     "message rate too high — slow down", server_log_);
                    // warn but continue processing this message
                } else if (rl == RateLimitResult::Suspend) {
                    serialise::error(ws, WsErrorCode::RateLimitExceeded,
                                     "connection suspended due to sustained rate excess", server_log_);
                    loop->defer([ws]() { ws->end(1008, "rate limit exceeded"); });
                    return;
                }
            }

            try {
                const auto j    = nlohmann::json::parse(msg);
                const auto type = j.value("type", "");

                // Lobby subscriptions — always handled regardless of game state
                if (type == "subscribe_lobby") {
                    lobby_gateway_.handle_subscribe(ws, j.value("lobby_id", ""),
                                                    loop, server_log_);
                    return;
                }
                if (type == "unsubscribe_lobby") {
                    lobby_gateway_.handle_unsubscribe(ws);
                    return;
                }
                // AGENT-CTX: leave_lobby is handled by WsServer (not GameSession)
                // because it requires DB writes (remove_player, delete_if_empty)
                // and slot-mapping cleanup. The game-loop thread only sees a
                // NetDisconnect after WsServer has already done the DB work.
                if (type == "leave_lobby") {
                    auto fields = parse::leave_lobby(j);
                    if (!fields) {
                        serialise::error(ws, WsErrorCode::MalformedMessage,
                                         "leave_lobby requires lobby_id", server_log_);
                        return;
                    }
                    handle_leave_lobby(ws, fields->lobby_id);
                    return;
                }
                if (type == "spectate_lobby") {
                    std::string sid = j.value("lobby_id", "");
                    const std::string code = j.value("lobby_code", "");
                    if (sid.empty() && !code.empty()) {
                        try {
                            auto handle = db_pool_.acquire();
                            pqxx::work txn(handle.get());
                            if (auto lobby = lobby_repo_.find_by_code(txn, code))
                                sid = lobby->id;
                        } catch (...) {}
                    }
                    if (sid.empty()) {
                        serialise::error(ws, WsErrorCode::MalformedMessage,
                                         "spectate_lobby requires lobby_id or lobby_code", server_log_);
                        return;
                    }
                    handle_spectate_lobby(ws, sid);
                    return;
                }

                // All game commands require an active session
                if (data->lobby_id.empty()) {
                    serialise::error(ws, WsErrorCode::RoundNotActive,
                                     "not in a game session", server_log_);
                    return;
                }
                auto session_it = active_sessions_.find(data->lobby_id);
                if (session_it == active_sessions_.end()) {
                    serialise::error(ws, WsErrorCode::RoundNotActive,
                                     "session not found", server_log_);
                    return;
                }
                auto& as        = session_it->second;
                const int32_t slot = data->player_slot;

                // script_log: API-lobby players only — WsServer passthrough to spectators.
                // Never touches the SPSC queue; forwarded directly to spectator_handles_.
                if (type == "script_log") {
                    if (data->auth_type != AuthType::ApiKey ||
                        as.lobby_mode_   != LobbyMode::API) return;
                    const auto msg_it = j.find("message");
                    if (msg_it == j.end() || !msg_it->is_string()) return;
                    std::string text = msg_it->get<std::string>();
                    text.erase(std::remove_if(text.begin(), text.end(),
                        [](unsigned char c) { return c < 0x20 && c != '\n'; }),
                        text.end());
                    if (text.size() > 500) text.resize(500);
                    const int64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    const std::string payload = nlohmann::json{
                        {"type",        "script_log"},
                        {"player_slot", slot},
                        {"message",     text},
                        {"timestamp",   ts},
                    }.dump();
                    for (auto& [spec_id, spec_ws] : as.spectator_handles_)
                        spec_ws->send(payload, uWS::OpCode::TEXT);
                    return;
                }

                // AGENT-CTX: Spectators share the same .message path as players
                // but must never mutate game state. Guard here (not per-branch) so
                // every future trading message type is automatically blocked.
                if (data->role == ConnectionRole::Spectator) {
                    if (type == "submit_order" || type == "nudge" ||
                        type == "cancel_order" || type == "vote_to_end") {
                        serialise::error(ws, WsErrorCode::SpectatorNotAllowed,
                                         "spectators cannot send game commands", server_log_);
                        return;
                    }
                    // Non-trading message types from spectators fall through and
                    // are silently dropped below (unknown type path).
                    return;
                }

                if (type == "submit_order") {
                    auto f = parse::submit_order(j);
                    if (!f) { serialise::error(ws, WsErrorCode::MalformedMessage, "bad submit_order", server_log_); return; }
                    auto side = parse::side(f->side);
                    if (!side) { serialise::error(ws, WsErrorCode::MalformedMessage, "bad side", server_log_); return; }
                    as.inbound->enqueue(NetSubmit{slot, f->suit, *side, f->price});
                } else if (type == "nudge") {
                    auto f = parse::nudge(j);
                    if (!f) { serialise::error(ws, WsErrorCode::MalformedMessage, "bad nudge", server_log_); return; }
                    auto side = parse::side(f->side);
                    if (!side) { serialise::error(ws, WsErrorCode::MalformedMessage, "bad side", server_log_); return; }
                    as.inbound->enqueue(NetNudge{slot, f->suit, *side});
                } else if (type == "cancel_order") {
                    auto f = parse::cancel_order(j);
                    if (!f) { serialise::error(ws, WsErrorCode::MalformedMessage, "bad cancel_order", server_log_); return; }
                    as.inbound->enqueue(NetCancel{slot, f->order_id});
                } else if (type == "vote_to_end") {
                    as.inbound->enqueue(NetVoteToEnd{slot});
                }
                // Unknown game-command types are silently dropped — prevents log
                // spam when old client versions send now-unknown messages.

            } catch (const nlohmann::json::exception&) {
                serialise::error(ws, WsErrorCode::MalformedMessage, "invalid JSON", server_log_);
            }
        },

        .close = [this](WsHandle ws, int /*code*/, std::string_view /*reason*/) {
            auto* data = ws->getUserData();
            if (!data->lobby_id.empty()) {
                auto it = active_sessions_.find(data->lobby_id);
                if (it != active_sessions_.end()) {
                    auto& as = it->second;
                    if (data->role == ConnectionRole::Spectator) {
                        auto sp_it = as.ws_to_spectator_.find(ws);
                        if (sp_it != as.ws_to_spectator_.end()) {
                            const int32_t spec_id = sp_it->second;
                            as.ws_to_spectator_.erase(sp_it);
                            as.spectator_handles_.erase(spec_id);
                            as.spectator_count_ = std::max(0, as.spectator_count_ - 1);
                            const std::string count_msg = nlohmann::json{
                                {"type","spectator_count"}, {"count", as.spectator_count_}
                            }.dump();
                            for (auto& [s, wh] : as.slot_to_ws_)
                                wh->send(count_msg, uWS::OpCode::TEXT);
                            for (auto& [sid, wh] : as.spectator_handles_)
                                wh->send(count_msg, uWS::OpCode::TEXT);
                            as.inbound->enqueue(NetSpectatorLeave{spec_id});
                        }
                    } else {
                        auto ws_it = as.ws_to_slot_.find(ws);
                        if (ws_it != as.ws_to_slot_.end()) {
                            const int32_t slot = ws_it->second;
                            as.ws_to_slot_.erase(ws);
                            as.slot_to_ws_.erase(slot);
                            as.inbound->enqueue(NetDisconnect{slot});
                        }
                    }
                }
            }
            lobby_gateway_.cleanup(ws);
        },
    });

    // ── /api/log forwarding endpoint ──────────────────────────────────────
    app.post("/api/log", [this](auto* res, auto* req) {
        const std::string allowed_origin = cfg_.cors_origin;
        std::string_view origin = req->getHeader("origin");
        if (origin != allowed_origin) {
            res->writeStatus("403 Forbidden")->end("");
            return;
        }
        auto body_buf = std::make_shared<std::string>();
        body_buf->reserve(4096);
        res->onData([res, body_buf, allowed_origin, this](std::string_view chunk, bool last) {
            constexpr std::size_t kMaxBody = 1 * 1024 * 1024;
            if (body_buf->size() + chunk.size() > kMaxBody) { res->close(); return; }
            *body_buf += chunk;
            if (!last) return;
            try {
                const auto arr = nlohmann::json::parse(*body_buf);
                if (arr.is_array()) {
                    for (const auto& entry : arr) {
                        const std::string level   = entry.value("level",     "INFO");
                        const std::string comp    = entry.value("component", "frontend");
                        const std::string message = entry.value("message",   "");
                        const std::string data_s  = entry.contains("data") && !entry["data"].is_null()
                            ? " data=" + entry["data"].dump() : "";
                        if      (level == "ERROR") frontend_log_.error(comp, message + data_s);
                        else if (level == "WARN")  frontend_log_.warn (comp, message + data_s);
                        else if (level == "DEBUG") frontend_log_.debug(comp, message + data_s);
                        else                       frontend_log_.info  (comp, message + data_s);
                    }
                }
            } catch (const std::exception& ex) {
                frontend_log_.warn("/api/log", std::string("parse error: ") + ex.what());
            }
            res->cork([res, allowed_origin]() {
                res->writeHeader("Content-Type", "text/plain")
                   ->writeHeader("Access-Control-Allow-Origin", allowed_origin)
                   ->end("ok");
            });
        });
        res->onAborted([body_buf]() {});
    });

    app.options("/api/log", [this](auto* res, auto* req) {
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

    // ── Outbound drain timer (16 ms) ──────────────────────────────────────
    // AGENT-CTX: We store `this` in the timer's ext memory (sizeof(void*) bytes).
    // The timer fires on the uWS event-loop thread so drain_all_on_loop() can
    // safely access active_sessions_ without a mutex. Fallthrough=0 means the
    // timer keeps the event loop alive — correct, we always want it running.
    auto* drain_timer = us_create_timer(
        reinterpret_cast<struct us_loop_t*>(loop), 0, sizeof(void*));
    *(WsServer**)us_timer_ext(drain_timer) = this;
    us_timer_set(drain_timer, [](struct us_timer_t* t) {
        auto* self = *(WsServer**)us_timer_ext(t);
        self->drain_all_on_loop();
    }, 16, 16);

    // Rate-limiter GC: remove expired suspension entries on the event-loop thread
    // (rate_limiter_ is not thread-safe by design — no mutex needed here).
    // Fire at suspend_seconds cadence so entries are cleaned up as soon as they expire.
    const unsigned int rl_gc_ms = static_cast<unsigned int>(
        cfg_.rate_limit.suspend_seconds) * 1000u;
    auto* rl_cleanup_timer = us_create_timer(
        reinterpret_cast<struct us_loop_t*>(loop), 0, sizeof(void*));
    *(WsServer**)us_timer_ext(rl_cleanup_timer) = this;
    us_timer_set(rl_cleanup_timer, [](struct us_timer_t* t) {
        auto* self = *(WsServer**)us_timer_ext(t);
        self->rate_limiter_.cleanup_expired();
    }, rl_gc_ms, rl_gc_ms);

    bool listen_ok = false;
    app.listen(cfg_.host, cfg_.port, [this, &listen_ok](auto* token) {
        if (token) {
            listen_ok = true;
            server_log_.info("startup",
                "listening on " + cfg_.host + ":" + std::to_string(cfg_.port));
            std::cout << "[server] listening on "
                      << cfg_.host << ":" << cfg_.port << '\n';
        } else {
            server_log_.error("startup",
                "failed to listen on port " + std::to_string(cfg_.port));
            std::cerr << "[server] failed to listen on port " << cfg_.port << '\n';
        }
    });
    if (!listen_ok)
        throw std::runtime_error("failed to listen on port " + std::to_string(cfg_.port));

    app.run();

    // Clean up event bus subscription before returning
    event_bus_.unsubscribe(game_start_sub_id_);
    us_timer_close(drain_timer);
    us_timer_close(rl_cleanup_timer);
    server_log_.info("shutdown", "server stopped");
}

// ─── create_session ───────────────────────────────────────────────────────
// Called on the uWS event-loop thread via loop->defer().
void WsServer::create_session(const std::string& lobby_id) {
    // Guard against duplicate creation (e.g. double-publish on event bus)
    if (active_sessions_.count(lobby_id)) {
        server_log_.warn("create_session",
                         "session already exists for lobby " + lobby_id);
        return;
    }

    std::vector<LobbyPlayer> players;
    std::string session_id;
    LobbyMode lobby_mode = LobbyMode::UI;
    try {
        auto handle = db_pool_.acquire();
        pqxx::work txn(handle.get());
        players = lobby_repo_.list_players(txn, lobby_id);
        if (players.empty()) {
            server_log_.warn("create_session", "no players in lobby " + lobby_id);
            return;
        }
        if (auto lobby = lobby_repo_.find_by_id(txn, lobby_id)) {
            lobby_mode = lobby->mode;
        }
        session_id = session_repo_.create_session(txn, lobby_id);
        // AGENT-CTX: Transition Starting→InGame here (not in HttpServer) because
        // WsServer is the authority for session lifecycle. HttpServer only initiates
        // the start (Waiting→Starting); WsServer confirms the game is actually live.
        const bool ok = lobby_repo_.transition_status(
            txn, lobby_id, LobbyStatus::Starting, LobbyStatus::InGame);
        if (!ok) {
            server_log_.warn("create_session",
                             "lobby " + lobby_id + " not in Starting state — skipping");
            return;
        }
        txn.commit();
    } catch (const std::exception& ex) {
        server_log_.error("create_session", std::string("DB error: ") + ex.what());
        return;
    }

    // Build SlotInfo from lobby player list (ordering is stable — list_players
    // returns rows in joined_at order, which gives deterministic slot assignment).
    std::vector<SlotInfo> slots;
    slots.reserve(players.size());
    for (const auto& p : players) {
        slots.push_back(SlotInfo{p.player_id, p.username,
                                 cfg_.scoring.starting_balance, false, true});
    }

    // Populate the session map entry before constructing GameSession so that
    // we can pass queue refs by reference (GameSession stores refs, not copies).
    auto& as        = active_sessions_[lobby_id];
    as.slots_        = slots;   // copy for WsServer's slot mapping
    as.lobby_mode_   = lobby_mode;
    as.inbound  = std::make_unique<moodycamel::ReaderWriterQueue<NetEvent>>(256);
    as.outbound = std::make_unique<moodycamel::ReaderWriterQueue<GameEvent>>(256);

    as.session = std::make_unique<GameSession>(
        session_id, lobby_id,
        as.slots_,          // copy: GameSession owns its own SlotInfo vector
        GameSessionContext{cfg_, server_log_, engine_log_, std::mt19937{rng_()}, db_pool_},
        *as.inbound, *as.outbound);

    as.session->start();
    as.inbound->enqueue(NetStartGame{});

    // Populate reverse-lookup so .open can route reconnecting players
    for (const auto& slot_info : as.slots_) {
        player_to_lobby_[slot_info.player_id] = lobby_id;
    }

    server_log_.info("create_session",
        "session " + session_id + " created for lobby " + lobby_id +
        " with " + std::to_string(slots.size()) + " players");
}

// ─── teardown_session ──────────────────────────────────────────────────────
// Called on the uWS event-loop thread (from drain_all_on_loop after GameDone).
void WsServer::teardown_session(const std::string& lobby_id) {
    auto it = active_sessions_.find(lobby_id);
    if (it == active_sessions_.end()) return;

    auto& as = it->second;
    as.session->shutdown();

    // Transition lobby InGame→Closed so it stops appearing in the active tab
    try {
        auto handle = db_pool_.acquire();
        pqxx::work txn(handle.get());
        lobby_repo_.transition_status(txn, lobby_id,
                                      LobbyStatus::InGame, LobbyStatus::Closed);
        txn.commit();
    } catch (const std::exception& ex) {
        server_log_.error("teardown_session",
                          std::string("DB error closing lobby: ") + ex.what());
    }

    for (const auto& slot_info : as.slots_) {
        player_to_lobby_.erase(slot_info.player_id);
    }
    active_sessions_.erase(it);
    server_log_.info("teardown_session", "session torn down for lobby " + lobby_id);
}

// ─── drain_all_on_loop ────────────────────────────────────────────────────
// Called every 16 ms on the uWS event-loop thread by the us_timer.
void WsServer::drain_all_on_loop() {
    // Collect done lobbies first — erasing during iteration is UB on unordered_map
    std::vector<std::string> done;

    for (auto& [lobby_id, as] : active_sessions_) {
        GameEvent ev;
        while (as.outbound->try_dequeue(ev)) {
            std::visit([&](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, GameBroadcast>) {
                    for (auto& [slot, ws] : as.slot_to_ws_)
                        ws->send(arg.json, uWS::OpCode::TEXT);
                    // AGENT-CTX: Spectators receive all broadcasts (market data,
                    // trade feed, balances) but never player-targeted messages.
                    for (auto& [sid, ws] : as.spectator_handles_)
                        ws->send(arg.json, uWS::OpCode::TEXT);
                } else if constexpr (std::is_same_v<T, GameTargeted>) {
                    auto wh = as.slot_to_ws_.find(arg.slot);
                    if (wh != as.slot_to_ws_.end())
                        wh->second->send(arg.json, uWS::OpCode::TEXT);
                } else if constexpr (std::is_same_v<T, GameSpectatorTargeted>) {
                    auto wh = as.spectator_handles_.find(arg.spectator_id);
                    if (wh != as.spectator_handles_.end())
                        wh->second->send(arg.json, uWS::OpCode::TEXT);
                } else if constexpr (std::is_same_v<T, GameSpectatorBroadcast>) {
                    for (auto& [sid, ws] : as.spectator_handles_)
                        ws->send(arg.json, uWS::OpCode::TEXT);
                } else if constexpr (std::is_same_v<T, GameDone>) {
                    done.push_back(lobby_id);
                }
            }, ev);
        }
    }

    for (const auto& lid : done) {
        teardown_session(lid);
    }
}

// ─── handle_leave_lobby ───────────────────────────────────────────────────
// Called on the uWS event-loop thread from the .message handler.
void WsServer::handle_leave_lobby(WsHandle ws, const std::string& lobby_id) {
    auto* data = ws->getUserData();
    if (data->player_id < 0) return;

    try {
        auto handle = db_pool_.acquire();
        pqxx::work txn(handle.get());
        lobby_repo_.remove_player(txn, lobby_id, data->player_id);
        // AGENT-CTX: delete_if_empty only runs for pre-game lobbies. If the
        // session is active the lobby row must stay alive (it's the FK parent for
        // game_sessions). Teardown_session closes the lobby when the game ends.
        if (!active_sessions_.count(lobby_id)) {
            lobby_repo_.delete_if_empty(txn, lobby_id);
        }
        txn.commit();
    } catch (const std::exception& ex) {
        server_log_.error("leave_lobby", std::string("DB error: ") + ex.what());
    }

    // Disconnect from active game session if applicable
    if (!data->lobby_id.empty() && data->lobby_id == lobby_id) {
        auto it = active_sessions_.find(lobby_id);
        if (it != active_sessions_.end()) {
            auto& as = it->second;
            auto ws_it = as.ws_to_slot_.find(ws);
            if (ws_it != as.ws_to_slot_.end()) {
                const int32_t slot = ws_it->second;
                as.ws_to_slot_.erase(ws);
                as.slot_to_ws_.erase(slot);
                as.inbound->enqueue(NetPermanentLeave{slot});
            }
        }
        data->lobby_id.clear();
        data->player_slot = -1;
    }

    // AGENT-CTX: Unsubscribe BEFORE publishing so the departing socket is removed
    // from the fan-out list first. LocalEventBus dispatches synchronously via
    // loop->defer(), but the defer callback checks subs_.count(ws) before sending,
    // so the order here is a belt-and-suspenders guard for future bus implementations
    // that might not have that check.
    lobby_gateway_.handle_unsubscribe(ws);

    // Broadcast departure to remaining lobby members.
    // AGENT-CTX: Only publish when this was a pre-game lobby socket (not a game
    // session socket that happened to carry a lobby_id). Active game sessions have
    // their own player_left equivalent (game_player_left) dispatched by GameSession.
    if (!active_sessions_.count(lobby_id) && data->player_id >= 0) {
        nlohmann::json ev;
        ev["type"]      = "player_left";
        ev["lobby_id"]  = lobby_id;
        ev["player_id"] = data->player_id;
        ev["username"]  = data->username;
        event_bus_.publish("lobby:" + lobby_id, ev.dump());
    }

    server_log_.info("leave_lobby", "player " + std::to_string(data->player_id) +
                     " left lobby " + lobby_id);
}

// ─── handle_spectate_lobby ────────────────────────────────────────────────
// Called on the uWS event-loop thread from the .message handler.
void WsServer::handle_spectate_lobby(WsHandle ws, const std::string& lobby_id) {
    auto* data = ws->getUserData();

    auto it = active_sessions_.find(lobby_id);
    if (it == active_sessions_.end()) {
        serialise::error(ws, WsErrorCode::RoundNotActive, "no active session for lobby", server_log_);
        return;
    }
    auto& as = it->second;

    // AGENT-CTX: Reject any socket that already holds a player slot in this
    // session. A player cannot downgrade to spectator mid-game; they must leave
    // the lobby first. Checking ws_to_slot (not player_to_lobby_) ensures the
    // guard fires only if this specific handle is registered, not just any
    // handle for this player_id.
    if (as.ws_to_slot_.count(ws)) {
        serialise::error(ws, WsErrorCode::SpectatorNotAllowed,
                         "already joined as player", server_log_);
        return;
    }

    // Prevent double-registration of the same handle as spectator.
    if (as.ws_to_spectator_.count(ws)) return;

    const int32_t spec_id = static_cast<int32_t>(data->player_id);
    data->role     = ConnectionRole::Spectator;
    data->lobby_id = lobby_id;

    as.spectator_handles_[spec_id] = ws;
    as.ws_to_spectator_[ws]        = spec_id;
    ++as.spectator_count_;

    // Inform the joining spectator of their role
    ws->send(nlohmann::json{
        {"type","player_hello"}, {"player_id", data->player_id}, {"role","spectator"}
    }.dump(), uWS::OpCode::TEXT);

    // Broadcast updated count to all players and all spectators.
    const std::string count_msg = nlohmann::json{
        {"type","spectator_count"}, {"count", as.spectator_count_}
    }.dump();
    for (auto& [slot, wh] : as.slot_to_ws_)
        wh->send(count_msg, uWS::OpCode::TEXT);
    for (auto& [sid, wh] : as.spectator_handles_)
        wh->send(count_msg, uWS::OpCode::TEXT);

    as.inbound->enqueue(NetSpectatorJoin{spec_id, data->username});

    server_log_.info("spectate_lobby",
        "spectator player_id=" + std::to_string(data->player_id) +
        " joined lobby " + lobby_id +
        " (count=" + std::to_string(as.spectator_count_) + ")");
}

} // namespace anjeer::server
