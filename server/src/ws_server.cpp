#include "server/ws_server.h"
#include "server/crypto_util.h"
#include "server/game_session_wire.h"

#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include <algorithm>
#include <chrono>
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
    : cfg_                  (cfg)
    , lobby_gateway_        (deps.lobby_gateway)
    , db_pool_              (deps.db_pool)
    , lobby_repo_           (deps.lobby_repo)
    , auth_service_         (deps.auth_service)
    , api_key_repo_         (deps.api_key_repo)
    , event_bus_            (deps.event_bus)
    , bot_manager_          (deps.bot_manager)
    , rate_limiter_         (RateLimitConfig{
          cfg.rate_limit.capacity,
          cfg.rate_limit.refill_rate,
          cfg.rate_limit.suspend_threshold,
          cfg.rate_limit.suspend_seconds})
    , server_log_           ("logs/server_logs.txt")
    , engine_log_           ("logs/engine_logs.txt")
    , frontend_log_         ("logs/frontend_logs.txt")
    , game_slots_repo_      (deps.db_pool)
    , rng_                  (std::random_device{}())
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

            std::string_view auth_hdr      = req->getHeader("authorization");
            std::string_view api_key_qp    = req->getQuery("api_key");
            std::string_view reconnect_qp  = req->getQuery("token");

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
            psd.player_id       = player_id;
            psd.auth_type       = auth_type;
            psd.pending_close   = pending_close;
            psd.username        = username;
            psd.reconnect_token = std::string(reconnect_qp);

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
                        // Defense-in-depth: if expiry already fired, don't attach.
                        // Primary guard is player_to_lobby_.erase on expiry; this catches
                        // any race where the map entry survived (e.g. mid-drain ordering).
                        if (as.available_slots_.count(slot) > 0) {
                            ws->send(nlohmann::json{
                                {"type","reconnect_window_expired"}}.dump(), uWS::OpCode::TEXT);
                            server_log_.warn("open",
                                "slot " + std::to_string(slot) + " expired — blocked auto-reattach");
                            // Fall through to lobby-socket path so useReconnect can send
                            // reconnect_game, which handle_reconnect_game will formally reject.
                        } else {
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
                                if (!attach_slot(ws, as, slot, lobby_id, loop)) {
                                    loop->defer([ws]() { ws->end(1008, "invalid reconnect token"); });
                                }
                                return;
                            }
                            } // switch
                        }
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
                    const std::string lid = j.value("lobby_id", "");
                    std::vector<BotPlayerEntry> bot_entries;
                    for (const auto& b : bot_manager_.get_bots(lid))
                        bot_entries.push_back({b.bot_uuid, b.username, b.difficulty});
                    lobby_gateway_.handle_subscribe(ws, lid, loop, server_log_, bot_entries);
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
                if (type == "add_bot") {
                    handle_add_bot(ws, j.value("lobby_id", ""),
                                   j.value("difficulty", ""));
                    return;
                }
                if (type == "remove_bot") {
                    handle_remove_bot(ws, j.value("lobby_id", ""),
                                      j.value("bot_uuid", ""));
                    return;
                }
                if (type == "join_queue") {
                    handle_join_queue(ws, j.value("lobby_id", ""), loop);
                    return;
                }
                if (type == "leave_queue") {
                    handle_leave_queue(ws, j.value("lobby_id", ""), loop);
                    return;
                }
                if (type == "reconnect_game") {
                    handle_reconnect_game(ws, j.value("lobby_id", ""),
                                          j.value("token", ""));
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
                        type == "cancel_order" || type == "start_next_round" ||
                        type == "end_game") {
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
                } else if (type == "start_next_round") {
                    // Only the current session owner may advance the round.
                    if (data->player_id == as.current_owner_player_id_)
                        as.inbound->enqueue(NetOwnerStartRound{});
                    // Silently drop if not the owner — client UI should hide the button.
                } else if (type == "end_game") {
                    if (data->player_id == as.current_owner_player_id_)
                        as.inbound->enqueue(NetOwnerEndGame{});
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
                            // Use reconnect-aware disconnect: starts per-slot timer in GameSession.
                            as.session->handle_player_disconnect(slot);
                            game_slots_repo_.mark_disconnected(as.session_id_, slot);
                            server_log_.info("close",
                                "slot " + std::to_string(slot) + " disconnected — reconnect window started");
                        } else if (as.queue_ && as.queue_->dequeue_by_ws(ws)) {
                            as.queue_->broadcast_positions(uWS::Loop::get());
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
    LobbyMode   lobby_mode            = LobbyMode::UI;
    bool        spawn_bots_on_leave   = false;
    std::string bot_spawn_difficulty  = "easy";
    int64_t     creator_id            = -1;
    try {
        auto handle = db_pool_.acquire();
        pqxx::work txn(handle.get());
        players = lobby_repo_.list_players(txn, lobby_id);
        if (players.empty()) {
            server_log_.warn("create_session", "no players in lobby " + lobby_id);
            return;
        }
        if (auto lobby = lobby_repo_.find_by_id(txn, lobby_id)) {
            lobby_mode           = lobby->mode;
            spawn_bots_on_leave  = lobby->spawn_bots_on_leave;
            bot_spawn_difficulty = lobby->bot_spawn_difficulty;
            creator_id           = lobby->creator_id;
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

    // Append bot slots after all human slots. Bot player_ids are negative so
    // they are never matched in the human .open / player_to_lobby_ path.
    const auto bot_list    = bot_manager_.get_bots(lobby_id);
    const int  human_count = static_cast<int>(slots.size());
    for (int i = 0; i < static_cast<int>(bot_list.size()); ++i) {
        const int64_t bot_pid = -(static_cast<int64_t>(human_count + i) + 1);
        slots.push_back(SlotInfo{bot_pid, bot_list[i].username,
                                 cfg_.scoring.starting_balance, false, true});
    }

    // Populate the session map entry before constructing GameSession so that
    // we can pass queue refs by reference (GameSession stores refs, not copies).
    auto& as        = active_sessions_[lobby_id];
    as.slots_        = slots;   // copy for WsServer's slot mapping
    as.lobby_mode_   = lobby_mode;
    as.inbound  = std::make_unique<moodycamel::ReaderWriterQueue<NetEvent>>(256);
    as.outbound = std::make_unique<moodycamel::ReaderWriterQueue<GameEvent>>(256);

    // Wire bot adapters and enqueue their NetConnect before NetStartGame so
    // GameSession registers all bot slots before the countdown begins.
    if (!bot_list.empty()) {
        std::unordered_map<std::string, int> slot_map;
        for (int i = 0; i < static_cast<int>(bot_list.size()); ++i)
            slot_map[bot_list[i].bot_uuid] = human_count + i;
        bot_manager_.attach_to_session(
            lobby_id, slot_map,
            cfg_.scoring.points_per_card,
            cfg_.scoring.pot_size / static_cast<int>(slots.size()),
            cfg_.game.round_duration_seconds,
            lobby_mode == LobbyMode::UI
        );
        for (int i = 0; i < static_cast<int>(bot_list.size()); ++i) {
            const int     slot    = human_count + i;
            const int64_t bot_pid = -(static_cast<int64_t>(slot) + 1);
            as.inbound->enqueue(NetConnect{slot, bot_pid, bot_list[i].username});
        }
    }

    as.spawn_bots_on_leave_      = spawn_bots_on_leave;
    as.bot_spawn_difficulty_     = bot_spawn_difficulty;
    as.session_id_               = session_id;
    as.queue_                    = std::make_unique<LobbyQueue>(cfg_.reconnect.max_queue_size);
    as.current_owner_player_id_  = creator_id;

    // Record each human slot as active in game_slots so payout reconciliation
    // can detect any bots that replaced disconnected players.
    for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
        if (slots[i].player_id >= 0) {
            game_slots_repo_.upsert_active(session_id, slots[i].player_id, i);
        }
    }

    GameSessionContext gs_ctx{cfg_, server_log_, engine_log_, std::mt19937{rng_()}, db_pool_};

    as.session = std::make_unique<GameSession>(
        session_id, lobby_id,
        as.slots_,          // copy: GameSession owns its own SlotInfo vector
        std::move(gs_ctx),
        *as.inbound, *as.outbound);

    as.session->start();
    as.inbound->enqueue(NetStartGame{});

    // Populate reverse-lookup so .open can route reconnecting players.
    // Skip bot slots (negative player_ids) — bots never reconnect.
    for (const auto& slot_info : as.slots_) {
        if (slot_info.player_id >= 0)
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

    bot_manager_.teardown_session(lobby_id);

    for (const auto& slot_info : as.slots_) {
        if (slot_info.player_id >= 0)
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
                    bot_manager_.dispatch_to_bots(lobby_id, arg.json, -1);
                } else if constexpr (std::is_same_v<T, GameTargeted>) {
                    auto wh = as.slot_to_ws_.find(arg.slot);
                    if (wh != as.slot_to_ws_.end())
                        wh->second->send(arg.json, uWS::OpCode::TEXT);
                    bot_manager_.dispatch_to_bots(lobby_id, arg.json, arg.slot);
                } else if constexpr (std::is_same_v<T, GameSpectatorTargeted>) {
                    auto wh = as.spectator_handles_.find(arg.spectator_id);
                    if (wh != as.spectator_handles_.end())
                        wh->second->send(arg.json, uWS::OpCode::TEXT);
                } else if constexpr (std::is_same_v<T, GameSpectatorBroadcast>) {
                    for (auto& [sid, ws] : as.spectator_handles_)
                        ws->send(arg.json, uWS::OpCode::TEXT);
                } else if constexpr (std::is_same_v<T, GameDone>) {
                    done.push_back(lobby_id);
                } else if constexpr (std::is_same_v<T, GameSpawnBot>) {
                    if (as.inbound && as.spawn_bots_on_leave_) {
                        bot_manager_.spawn_replacement(
                            lobby_id,
                            BotSpawnContext{
                                arg.slot,
                                arg.hand,
                                arg.balance,
                                arg.remaining_s,
                                as.bot_spawn_difficulty_,
                                cfg_.scoring.points_per_card,
                                cfg_.scoring.pot_size / static_cast<int>(as.slots_.size()),
                                cfg_.game.round_duration_seconds,
                            },
                            *as.inbound,
                            as.lobby_mode_ == LobbyMode::UI
                        );
                        // Notify all clients so they un-grey the slot and update the roster.
                        auto bots = bot_manager_.get_bots(lobby_id);
                        for (const auto& b : bots) {
                            if (b.slot != arg.slot) continue;
                            nlohmann::json n;
                            n["type"]           = "game_bot_joined";
                            n["player_slot"]    = arg.slot;
                            n["username"]       = b.username;
                            n["bot_uuid"]       = b.bot_uuid;
                            n["bot_difficulty"] = b.difficulty;
                            std::string payload = n.dump();
                            for (auto& [slot, ws] : as.slot_to_ws_)
                                ws->send(payload, uWS::OpCode::TEXT);
                            for (auto& [sid, ws] : as.spectator_handles_)
                                ws->send(payload, uWS::OpCode::TEXT);
                            break;
                        }
                    }
                } else if constexpr (std::is_same_v<T, GameReconnectExpired>) {
                    // Reconnect window expired: send reconnect_window_expired to the
                    // stale socket if it somehow re-opened before the timer fired,
                    // mark slot available for queue admission, and clean up DB token.
                    auto wh = as.slot_to_ws_.find(arg.slot);
                    if (wh != as.slot_to_ws_.end())
                        wh->second->send(nlohmann::json{
                            {"type","reconnect_window_expired"}}.dump(), uWS::OpCode::TEXT);
                    as.available_slots_.insert(arg.slot);
                    game_slots_repo_.mark_expired(as.session_id_, arg.slot);
                    server_log_.info("reconnect",
                        "slot " + std::to_string(arg.slot) + " reconnect window expired");
                    // Mark inactive before transfer_ownership so it skips this slot.
                    as.slots_[arg.slot].active = false;
                    // Immediately free this player's active-lobby binding so they can
                    // join another lobby without waiting for session teardown.
                    const int64_t expired_pid = as.slots_[arg.slot].player_id;
                    if (expired_pid >= 0) {
                        player_to_lobby_.erase(expired_pid);
                        if (expired_pid == as.current_owner_player_id_)
                            transfer_ownership(as, lobby_id);
                    }
                } else if constexpr (std::is_same_v<T, GameRoundStarted>) {
                    // Phase 1: Displace bots to make room for queue players.
                    // For each bot displaced: stop the BotAdapter, deactivate the slot in
                    // GameSession, and mark it available. Called in the same threading window
                    // as admit_from_queue (game-loop briefly idle) — same safety model.
                    if (as.queue_ && as.queue_->size() > 0 && bot_manager_.has_bots(lobby_id)) {
                        int need = static_cast<int>(as.queue_->size())
                                 - static_cast<int>(as.available_slots_.size());
                        while (need > 0) {
                            const int bot_slot = bot_manager_.get_displaceable_bot_slot(
                                lobby_id, *as.session);
                            if (bot_slot < 0) break;
                            bot_manager_.remove_bot_for_slot(lobby_id, bot_slot);
                            as.session->deactivate_bot_slot(bot_slot);
                            as.available_slots_.insert(bot_slot);
                            as.slots_[bot_slot].active = false;
                            server_log_.info("queue",
                                "bot displaced from slot " + std::to_string(bot_slot) +
                                " to admit queue player");
                            --need;
                        }
                    }

                    // Phase 2: Drain queue into available slots at round boundary.
                    // AGENT-CTX: admit_from_queue mutates slots_ on the event-loop thread
                    // while the game-loop thread is (briefly) idle after emitting this event.
                    // This is safe in practice for single-node but flagged for Slice 16
                    // where a proper NetAdmitQueue message should replace the direct call.
                    if (!as.available_slots_.empty() && as.queue_ && as.queue_->size() > 0) {
                        const int count = static_cast<int>(
                            std::min(as.available_slots_.size(),
                                     static_cast<size_t>(as.queue_->size())));
                        auto entries = as.queue_->drain(count);
                        std::vector<SlotAdmitInfo> admits;
                        // Capture bot UUIDs before slots are re-assigned so we can
                        // broadcast game_bot_replaced after admission.
                        std::unordered_map<int, std::string> displaced_bot_uuids;
                        auto slot_it = as.available_slots_.begin();
                        for (auto& entry : entries) {
                            const int slot = *slot_it++;
                            const std::string bot_uuid =
                                bot_manager_.bot_uuid_for_slot(lobby_id, slot);
                            if (!bot_uuid.empty()) {
                                // Safety net: remove bot if not already done in Phase 1.
                                bot_manager_.remove_bot_for_slot(lobby_id, slot);
                                as.session->deactivate_bot_slot(slot);
                                displaced_bot_uuids[slot] = bot_uuid;
                            }
                            admits.push_back(SlotAdmitInfo{
                                slot,
                                entry.player_id,
                                entry.username});
                        }
                        as.session->admit_from_queue(admits);
                        // Wire sockets and update WsServer maps for admitted players.
                        for (int i = 0; i < static_cast<int>(admits.size()); ++i) {
                            const auto& info  = admits[i];
                            const auto& entry = entries[i];
                            const int   slot  = info.slot_index;
                            as.slot_to_ws_[slot]          = entry.ws;
                            as.ws_to_slot_[entry.ws]      = slot;
                            as.slots_[slot].player_id     = info.player_id;
                            as.slots_[slot].username      = info.username;
                            as.slots_[slot].active        = true;
                            player_to_lobby_[info.player_id] = lobby_id;
                            auto* d = entry.ws->getUserData();
                            d->lobby_id    = lobby_id;
                            d->player_slot = slot;
                            as.available_slots_.erase(slot);
                            // Issue a fresh in-memory reconnect token for this slot.
                            const std::string q_token = generate_reconnect_token();
                            const int64_t q_ttl_s = static_cast<int64_t>(cfg_.reconnect.token_ttl_seconds);
                            const int64_t q_exp_ms =
                                std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch()).count()
                                + q_ttl_s * 1000;
                            as.slot_tokens_[slot] = q_token;
                            entry.ws->send(nlohmann::json{
                                {"type","queue_admitted"}, {"slot_index", slot}
                            }.dump(), uWS::OpCode::TEXT);
                            entry.ws->send(nlohmann::json{
                                {"type",       "reconnect_token"},
                                {"token",      q_token},
                                {"expires_at", q_exp_ms},
                            }.dump(), uWS::OpCode::TEXT);
                            game_slots_repo_.upsert_active(
                                as.session_id_, entry.player_id, slot);
                            // Broadcast bot replacement so clients clear the bot from
                            // departedSlots and update the roster with the new player.
                            auto bot_it = displaced_bot_uuids.find(slot);
                            const std::string bot_uuid = bot_it != displaced_bot_uuids.end()
                                ? bot_it->second : "";
                            if (!bot_uuid.empty()) {
                                const std::string rep_payload = nlohmann::json{
                                    {"type",          "game_bot_replaced"},
                                    {"slot_index",    slot},
                                    {"bot_uuid",      bot_uuid},
                                    {"new_player_id", info.player_id},
                                    {"username",      info.username},
                                }.dump();
                                for (auto& [s, wh] : as.slot_to_ws_)
                                    wh->send(rep_payload, uWS::OpCode::TEXT);
                                for (auto& [sid, wh] : as.spectator_handles_)
                                    wh->send(rep_payload, uWS::OpCode::TEXT);
                            }
                            server_log_.info("queue",
                                "player " + entry.username + " admitted into slot " +
                                std::to_string(slot));
                        }
                    }
                }
            }, ev);
        }
        bot_manager_.drain_bot_actions(lobby_id, *as.inbound);
    }

    for (const auto& lid : done) {
        teardown_session(lid);
    }
}

// ─── transfer_ownership ───────────────────────────────────────────────────
// Finds the lowest-indexed active real player and makes them the new owner.
// If no real players remain, tears down the session instead.
// Always called on the uWS event-loop thread.
void WsServer::transfer_ownership(ActiveSession& as, const std::string& lobby_id) {
    int64_t     new_owner_id   = -1;
    std::string new_owner_name;
    for (const auto& s : as.slots_) {
        if (s.active && s.player_id >= 0) {
            new_owner_id   = s.player_id;
            new_owner_name = s.username;
            break;
        }
    }
    if (new_owner_id < 0) {
        server_log_.info("owner", "no real players remain — tearing down lobby " + lobby_id);
        teardown_session(lobby_id);
        return;
    }
    as.current_owner_player_id_ = new_owner_id;
    const std::string msg = nlohmann::json{
        {"type",               "lobby_owner_changed"},
        {"new_owner_player_id", new_owner_id},
        {"new_owner_username",  new_owner_name}
    }.dump();
    for (auto& [slot, wh] : as.slot_to_ws_)
        wh->send(msg, uWS::OpCode::TEXT);
    for (auto& [sid, wh] : as.spectator_handles_)
        wh->send(msg, uWS::OpCode::TEXT);
    server_log_.info("owner",
        "ownership transferred to " + new_owner_name +
        " (id=" + std::to_string(new_owner_id) + ") in lobby " + lobby_id);
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
                // Mark inactive before transfer_ownership so it skips this slot.
                as.slots_[slot].active = false;
                // Immediately free the player's active-lobby binding so they can
                // join another lobby without waiting for session teardown.
                player_to_lobby_.erase(data->player_id);
                if (data->player_id == as.current_owner_player_id_)
                    transfer_ownership(as, lobby_id);
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

// ─── handle_add_bot ───────────────────────────────────────────────────────
void WsServer::handle_add_bot(WsHandle ws, const std::string& lobby_id,
                               const std::string& difficulty) {
    auto* data = ws->getUserData();

    if (lobby_id.empty() || difficulty.empty()) {
        serialise::error(ws, WsErrorCode::MalformedMessage,
                         "add_bot requires lobby_id and difficulty", server_log_);
        return;
    }

    // Parse difficulty
    anjeer::engine::BotDifficulty diff;
    if      (difficulty == "easy")   diff = anjeer::engine::BotDifficulty::Easy;
    else if (difficulty == "medium") diff = anjeer::engine::BotDifficulty::Medium;
    else if (difficulty == "hard")   diff = anjeer::engine::BotDifficulty::Hard;
    else {
        serialise::error(ws, WsErrorCode::MalformedMessage,
                         "difficulty must be easy, medium, or hard", server_log_);
        return;
    }

    std::vector<LobbyPlayer> db_players;
    std::optional<Lobby> opt_lobby;
    try {
        auto handle = db_pool_.acquire();
        pqxx::work txn(handle.get());
        opt_lobby  = lobby_repo_.find_by_id(txn, lobby_id);
        if (opt_lobby) db_players = lobby_repo_.list_players(txn, lobby_id);
    } catch (const std::exception& ex) {
        server_log_.error("add_bot", std::string("DB error: ") + ex.what());
        serialise::error(ws, WsErrorCode::MalformedMessage, "internal error", server_log_);
        return;
    }

    if (!opt_lobby) {
        serialise::error(ws, WsErrorCode::LobbyNotFound, "lobby not found", server_log_);
        return;
    }
    if (opt_lobby->creator_id != data->player_id) {
        serialise::error(ws, WsErrorCode::NotLobbyOwner,
                         "only the lobby owner can add bots", server_log_);
        return;
    }
    if (opt_lobby->status != LobbyStatus::Waiting) {
        serialise::error(ws, WsErrorCode::LobbyAlreadyStarted,
                         "lobby has already started", server_log_);
        return;
    }

    const int total      = static_cast<int>(db_players.size())
                         + static_cast<int>(bot_manager_.get_bots(lobby_id).size());
    const int open_slots = opt_lobby->max_players - total;
    if (open_slots <= 0) {
        serialise::error(ws, WsErrorCode::BotLimitReached, "lobby is full", server_log_);
        return;
    }

    auto [ok, result] = bot_manager_.add_bot(lobby_id, diff, open_slots);
    if (!ok) {
        serialise::error(ws, WsErrorCode::BotLimitReached, result, server_log_);
        return;
    }

    try {
        auto handle = db_pool_.acquire();
        pqxx::work txn(handle.get());
        lobby_repo_.adjust_bot_count(txn, lobby_id, +1);
        txn.commit();
    } catch (const std::exception& ex) {
        // Non-fatal: bot is live in BotManager but DB counter may be stale.
        // bot_count divergence is detectable by comparing BotManager::get_bots() with
        // the DB value. Log enough context to diagnose without a full investigation.
        server_log_.error("add_bot",
            "bot_count update failed lobby=" + lobby_id +
            " bot_uuid=" + result +
            " err=" + ex.what());
    }

    const std::string& bot_uuid = result;
    const auto bots = bot_manager_.get_bots(lobby_id);
    std::string username;
    for (const auto& b : bots)
        if (b.bot_uuid == bot_uuid) { username = b.username; break; }

    nlohmann::json ev{
        {"type",           "player_joined"},
        {"lobby_id",       lobby_id},
        {"player_id",      nullptr},
        {"bot_uuid",       bot_uuid},
        {"username",       username},
        {"is_bot",         true},
        {"bot_difficulty", difficulty},
        {"joined_at",      ""},
        {"player_count",   total + 1},
    };
    event_bus_.publish("lobby:" + lobby_id, ev.dump());
    server_log_.info("add_bot", "added " + difficulty + " bot to lobby " + lobby_id);
}

// ─── handle_remove_bot ────────────────────────────────────────────────────
void WsServer::handle_remove_bot(WsHandle ws, const std::string& lobby_id,
                                  const std::string& bot_uuid) {
    auto* data = ws->getUserData();

    if (lobby_id.empty() || bot_uuid.empty()) {
        serialise::error(ws, WsErrorCode::MalformedMessage,
                         "remove_bot requires lobby_id and bot_uuid", server_log_);
        return;
    }

    std::optional<Lobby> opt_lobby;
    int db_player_count = 0;
    try {
        auto handle = db_pool_.acquire();
        pqxx::work txn(handle.get());
        opt_lobby = lobby_repo_.find_by_id(txn, lobby_id);
        if (opt_lobby)
            db_player_count = static_cast<int>(
                lobby_repo_.list_players(txn, lobby_id).size());
    } catch (const std::exception& ex) {
        server_log_.error("remove_bot", std::string("DB error: ") + ex.what());
        serialise::error(ws, WsErrorCode::MalformedMessage, "internal error", server_log_);
        return;
    }

    if (!opt_lobby) {
        serialise::error(ws, WsErrorCode::LobbyNotFound, "lobby not found", server_log_);
        return;
    }
    if (opt_lobby->creator_id != data->player_id) {
        serialise::error(ws, WsErrorCode::NotLobbyOwner,
                         "only the lobby owner can remove bots", server_log_);
        return;
    }
    if (opt_lobby->status != LobbyStatus::Waiting) {
        serialise::error(ws, WsErrorCode::LobbyAlreadyStarted,
                         "cannot remove bots after game starts", server_log_);
        return;
    }

    // Find username before removal for the broadcast
    std::string username;
    for (const auto& b : bot_manager_.get_bots(lobby_id))
        if (b.bot_uuid == bot_uuid) { username = b.username; break; }

    if (!bot_manager_.remove_bot(lobby_id, bot_uuid)) {
        serialise::error(ws, WsErrorCode::MalformedMessage,
                         "bot not found in lobby", server_log_);
        return;
    }

    try {
        auto handle = db_pool_.acquire();
        pqxx::work txn(handle.get());
        lobby_repo_.adjust_bot_count(txn, lobby_id, -1);
        txn.commit();
    } catch (const std::exception& ex) {
        // Non-fatal: bot is removed from BotManager but DB counter may be stale.
        server_log_.error("remove_bot",
            "bot_count update failed lobby=" + lobby_id +
            " bot_uuid=" + bot_uuid +
            " err=" + ex.what());
    }

    const int new_total = db_player_count
                        + static_cast<int>(bot_manager_.get_bots(lobby_id).size());
    nlohmann::json ev{
        {"type",         "player_left"},
        {"lobby_id",     lobby_id},
        {"player_id",    nullptr},
        {"bot_uuid",     bot_uuid},
        {"username",     username},
        {"is_bot",       true},
        {"player_count", new_total},
    };
    event_bus_.publish("lobby:" + lobby_id, ev.dump());
    server_log_.info("remove_bot", "removed bot " + bot_uuid + " from lobby " + lobby_id);
}

// ─── attach_slot ──────────────────────────────────────────────────────────────
// Shared connect/reconnect logic called from .open and handle_reconnect_game.
// If data->reconnect_token is non-empty, validates it; returns false on failure.
// Always creates a fresh token, updates WsServer maps, and calls handle_player_reattach.
// AGENT-CTX: Using handle_player_reattach for ALL slot connections (initial and
// reconnect) keeps a single code path. handle_reconnect_reattach sends a full
// snapshot in RoundActive and is a no-op (just marks connected=true) otherwise,
// matching what we need for initial lobby-phase connects too.
bool WsServer::attach_slot(WsHandle ws, ActiveSession& as,
                            int32_t slot, const std::string& lobby_id,
                            uWS::Loop* /*loop*/) {
    auto* data = ws->getUserData();

    // Evict any stale handle already registered for this slot.
    auto old_it = as.slot_to_ws_.find(slot);
    if (old_it != as.slot_to_ws_.end()) {
        as.ws_to_slot_.erase(old_it->second);
    }

    // Validate reconnect token against the in-memory slot map — no DB round-trip.
    // The token is slot-scoped: player_id + lobby_id are already implied by the slot.
    if (!data->reconnect_token.empty()) {
        auto it = as.slot_tokens_.find(slot);
        if (it == as.slot_tokens_.end() || it->second != data->reconnect_token) {
            serialise::error(ws, WsErrorCode::MalformedMessage,
                             "reconnect token invalid or expired", server_log_);
            return false;
        }
    }

    const bool    was_reconnect  = !data->reconnect_token.empty();
    const int64_t token_ttl_s    = static_cast<int64_t>(cfg_.reconnect.token_ttl_seconds);
    const std::string new_token  = generate_reconnect_token();
    const int64_t expires_at_ms  =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()
        + token_ttl_s * 1000;
    as.slot_tokens_[slot] = new_token;

    as.slot_to_ws_[slot] = ws;
    as.ws_to_slot_[ws]   = slot;
    data->lobby_id        = lobby_id;
    data->player_slot     = slot;

    // queue_admitted first: any WS (useQueueSocket or useWebSocket) can use this
    // to detect slot assignment and navigate to the game page.
    ws->send(nlohmann::json{
        {"type","queue_admitted"}, {"slot_index", slot}
    }.dump(), uWS::OpCode::TEXT);

    // player_hello must arrive before game_state_snapshot so the client
    // initialises its player_id before processing the snapshot fields.
    ws->send(nlohmann::json{{"type","player_hello"}, {"player_id", data->player_id}
    }.dump(), uWS::OpCode::TEXT);

    // Inform the attaching player of the current session owner so they know
    // whether to render owner controls in the inter-round screen.
    if (as.current_owner_player_id_ >= 0) {
        std::string owner_username;
        for (const auto& s : as.slots_) {
            if (s.player_id == as.current_owner_player_id_) { owner_username = s.username; break; }
        }
        ws->send(nlohmann::json{
            {"type",               "lobby_owner_changed"},
            {"new_owner_player_id", as.current_owner_player_id_},
            {"new_owner_username",  owner_username}
        }.dump(), uWS::OpCode::TEXT);
    } else {
        server_log_.warn("attach_slot", "no owner set for session in lobby " + lobby_id);
    }

    as.session->handle_player_reattach(slot, new_token, expires_at_ms);
    game_slots_repo_.upsert_active(as.session_id_, data->player_id, slot);

    server_log_.info("open",
        "player " + std::to_string(data->player_id) + " attached to slot " +
        std::to_string(slot) + " in lobby " + lobby_id +
        (was_reconnect ? " (reattach)" : " (initial)"));
    data->reconnect_token.clear();  // consumed; avoid re-use on next call
    return true;
}

// ─── handle_reconnect_game ────────────────────────────────────────────────────
// Handles explicit in-session reattach via WS message (client-initiated, e.g.
// when the tab stayed open but the connection dropped momentarily).
void WsServer::handle_reconnect_game(WsHandle ws,
                                      const std::string& lobby_id,
                                      const std::string& token) {
    auto* data = ws->getUserData();

    if (lobby_id.empty() || token.empty()) {
        serialise::error(ws, WsErrorCode::MalformedMessage,
                         "reconnect_game requires lobby_id and token", server_log_);
        return;
    }

    auto session_it = active_sessions_.find(lobby_id);
    if (session_it == active_sessions_.end()) {
        serialise::error(ws, WsErrorCode::RoundNotActive,
                         "no active session for lobby", server_log_);
        return;
    }
    auto& as = session_it->second;

    // Find this player's slot
    int32_t slot = -1;
    for (int i = 0; i < static_cast<int>(as.slots_.size()); ++i) {
        if (as.slots_[i].player_id == data->player_id) { slot = i; break; }
    }
    if (slot < 0) {
        serialise::error(ws, WsErrorCode::MalformedMessage,
                         "player has no slot in this session", server_log_);
        return;
    }

    // If the slot is available for queue admission the reconnect window already
    // expired and a bot has taken over. Block the attach and tell the client so
    // the expiry overlay is shown instead of attaching the player to the bot.
    // AGENT-CTX: available_slots_ is inserted on GameReconnectExpired and erased
    // only when a queued player is admitted, so it is a reliable liveness guard.
    if (as.available_slots_.count(slot) > 0) {
        ws->send(nlohmann::json{{"type","reconnect_window_expired"}}.dump(),
                 uWS::OpCode::TEXT);
        server_log_.info("reconnect",
            "slot " + std::to_string(slot) + " window already expired — blocked");
        return;
    }

    data->reconnect_token = token;

    uWS::Loop* loop = uWS::Loop::get();
    if (!attach_slot(ws, as, slot, lobby_id, loop)) {
        loop->defer([ws]() { ws->end(1008, "invalid reconnect token"); });
    }
}

// ─── handle_join_queue ────────────────────────────────────────────────────────
// Enqueues a player for an active session. Fails gracefully if queue is full
// or the player is already waiting.
void WsServer::handle_join_queue(WsHandle ws, const std::string& lobby_id,
                                  uWS::Loop* loop) {
    auto* data = ws->getUserData();

    if (lobby_id.empty()) {
        serialise::error(ws, WsErrorCode::MalformedMessage,
                         "join_queue requires lobby_id", server_log_);
        return;
    }

    auto it = active_sessions_.find(lobby_id);
    if (it == active_sessions_.end()) {
        serialise::error(ws, WsErrorCode::RoundNotActive,
                         "no active session for lobby", server_log_);
        return;
    }
    auto& as = it->second;

    if (as.queue_->has(data->player_id)) {
        // Already in queue — send a refreshed position rather than an error.
        const int pos = as.queue_->position_of(data->player_id);
        ws->send(nlohmann::json{
            {"type","queue_joined"}, {"position", pos}, {"queue_size", as.queue_->size()}
        }.dump(), uWS::OpCode::TEXT);
        return;
    }

    const int pos = as.queue_->enqueue(data->player_id, data->username, ws);
    if (pos < 0) {
        ws->send(nlohmann::json{
            {"type","queue_overflow"}, {"lobby_id", lobby_id}
        }.dump(), uWS::OpCode::TEXT);
        server_log_.info("queue", "queue full for lobby " + lobby_id +
                         " — sent overflow to player " + std::to_string(data->player_id));
        return;
    }

    data->lobby_id = lobby_id;  // allows .close to find the right session queue
    ws->send(nlohmann::json{
        {"type","queue_joined"}, {"position", pos}, {"queue_size", as.queue_->size()}
    }.dump(), uWS::OpCode::TEXT);
    as.queue_->broadcast_positions(loop);

    server_log_.info("queue", "player " + std::to_string(data->player_id) + " enqueued pos=" +
                     std::to_string(pos) + " lobby=" + lobby_id);
}

// ─── handle_leave_queue ───────────────────────────────────────────────────────
void WsServer::handle_leave_queue(WsHandle ws, const std::string& lobby_id,
                                   uWS::Loop* loop) {
    auto* data = ws->getUserData();

    if (lobby_id.empty()) {
        serialise::error(ws, WsErrorCode::MalformedMessage,
                         "leave_queue requires lobby_id", server_log_);
        return;
    }

    auto it = active_sessions_.find(lobby_id);
    if (it == active_sessions_.end()) return;  // session gone — silently OK
    auto& as = it->second;

    as.queue_->dequeue(data->player_id);
    data->lobby_id.clear();
    as.queue_->broadcast_positions(loop);

    ws->send(nlohmann::json{{"type","queue_left"}}.dump(), uWS::OpCode::TEXT);
    server_log_.info("queue", "player " + std::to_string(data->player_id) + " left queue lobby=" + lobby_id);
}

} // namespace anjeer::server
