#include "server/http_server.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>

#include "server/api_key_repo.h"
#include "server/crypto_util.h"
#include "server/lobby_repo.h"

#include <nlohmann/json.hpp>
#include <openssl/rand.h>

// crow/middlewares/cors.h is not included by crow.h — it must be explicitly included.
#include <crow.h>
#include <crow/middlewares/cors.h>

namespace anjeer::server {

// ---------------------------------------------------------------------------
// Anonymous-namespace helpers
// ---------------------------------------------------------------------------

namespace {

// Crow's CookieParser requires passing the typed app ref into lambdas, which
// leaks crow::App<CORSHandler> into the header — so we parse cookies manually.
std::string read_cookie(const crow::request& req, std::string_view name)
{
    const std::string header = req.get_header_value("Cookie");
    if (header.empty()) return {};

    std::string_view sv(header);
    while (!sv.empty()) {
        while (!sv.empty() && sv.front() == ' ') sv.remove_prefix(1);

        const auto semi = sv.find(';');
        const auto tok  = semi != std::string_view::npos ? sv.substr(0, semi) : sv;
        sv              = semi != std::string_view::npos ? sv.substr(semi + 1)
                                                         : std::string_view{};

        const auto eq = tok.find('=');
        if (eq == std::string_view::npos) continue;
        if (tok.substr(0, eq) == name) return std::string(tok.substr(eq + 1));
    }
    return {};
}

// Converts a system_clock time_point to an ISO-8601 UTC string.
std::string tp_to_iso8601(std::chrono::system_clock::time_point tp)
{
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

crow::response make_error(int http_code, const std::string& error_code)
{
    nlohmann::json j;
    j["error"] = error_code;
    crow::response res(http_code, j.dump());
    res.set_header("Content-Type", "application/json");
    return res;
}

crow::response unauthorized(const std::string& error_code)
{
    return make_error(401, error_code);
}

nlohmann::json lobby_view_json(const LobbyView& lv)
{
    nlohmann::json j;
    j["id"]                    = lv.lobby.id;
    j["code"]                  = lv.lobby.code;
    j["creator_id"]            = lv.lobby.creator_id;
    j["status"]                = lobby_status_string(lv.lobby.status);
    j["mode"]                  = lobby_mode_string(lv.lobby.mode);
    j["min_players"]           = lv.lobby.min_players;
    j["max_players"]           = lv.lobby.max_players;
    j["player_count"]          = lv.player_count;
    j["created_at"]            = lv.lobby.created_at;
    j["spawn_bots_on_leave"]   = lv.lobby.spawn_bots_on_leave;
    j["bot_spawn_difficulty"]  = lv.lobby.bot_spawn_difficulty;
    return j;
}

std::variant<Player, crow::response> require_auth(
        const crow::request& req,
        AuthService&         auth_service,
        DbPool&              db_pool,
        PlayerRepo&          player_repo)
{
    const auto access_token = read_cookie(req, "access_token");
    if (access_token.empty()) return unauthorized("UNAUTHORIZED");
    const auto player_id = auth_service.validate_access_token(access_token);
    if (!player_id.has_value()) return unauthorized("TOKEN_EXPIRED");
    auto handle = db_pool.acquire();
    pqxx::work txn(handle.get());
    const auto player = player_repo.find_by_id(txn, *player_id);
    if (!player.has_value()) return unauthorized("UNAUTHORIZED");
    return *player;
}

// Overload that also accepts API key Bearer tokens, for endpoints used by scripts.
std::variant<Player, crow::response> require_auth(
        const crow::request& req,
        AuthService&         auth_service,
        DbPool&              db_pool,
        PlayerRepo&          player_repo,
        ApiKeyRepo&          api_key_repo)
{
    const std::string auth_header = req.get_header_value("Authorization");
    if (auth_header.substr(0, 7) == "Bearer ") {
        const std::string key = auth_header.substr(7);
        const std::string hash = sha256_hex(key);
        auto handle = db_pool.acquire();
        pqxx::work txn(handle.get());
        const auto record = api_key_repo.find_valid_by_hash(txn, hash);
        if (!record) return unauthorized("API_KEY_INVALID");
        const auto player = player_repo.find_by_id(txn, record->player_id);
        if (!player) return unauthorized("UNAUTHORIZED");
        return *player;
    }
    return require_auth(req, auth_service, db_pool, player_repo);
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

HttpServer::HttpServer(const ServerConfig& config, HttpServerDeps deps)
    : config_               (config)
    , auth_service_         (deps.auth_service)
    , player_repo_          (deps.player_repo)
    , lobby_repo_           (deps.lobby_repo)
    , keybinds_repo_        (deps.keybinds_repo)
    , api_key_repo_         (deps.api_key_repo)
    , spectate_token_repo_  (deps.spectate_token_repo)
    , event_bus_            (deps.event_bus)
    , db_pool_              (deps.db_pool)
    , http_log_             ("logs/http_logs.txt")
{
    providers_["github"] = std::make_unique<GitHubOAuthProvider>(config.auth.github);
    providers_["google"] = std::make_unique<GoogleOAuthProvider>(config.auth.google);
}

void HttpServer::run()
{
    crow::App<crow::CORSHandler> app;

    // origin() must be a specific value (not "*") when allow_credentials() is set —
    // browsers reject "Access-Control-Allow-Origin: *" with credentials.
    auto& cors = app.get_middleware<crow::CORSHandler>();
    cors.global()
        .origin(config_.cors_origin)
        .methods(crow::HTTPMethod::Get,
                 crow::HTTPMethod::Post,
                 crow::HTTPMethod::Put,
                 crow::HTTPMethod::Delete,
                 crow::HTTPMethod::Options)
        .headers("Content-Type", "Cookie")
        .allow_credentials();

    register_auth_routes(app);
    register_player_routes(app);
    register_lobby_routes(app);
    register_keybinds_routes(app);
    register_api_key_routes(app);
    register_spectate_routes(app);
    register_examples_routes(app);
    register_config_routes(app);

    app.loglevel(crow::LogLevel::Warning);
    http_log_.info("startup", "listening on :" + std::to_string(config_.http_port));
    app.port(config_.http_port).run();
}

template<typename App>
void HttpServer::register_auth_routes(App& app)
{
    CROW_ROUTE(app, "/auth/github")
    ([this](const crow::request&, crow::response& res) {
        const auto state = generate_state();
        add_state(state, "github");
        res.code = 302;
        res.add_header("Location", providers_.at("github")->authorization_url(state));
        res.end();
    });

    CROW_ROUTE(app, "/auth/google")
    ([this](const crow::request&, crow::response& res) {
        const auto state = generate_state();
        add_state(state, "google");
        res.code = 302;
        res.add_header("Location", providers_.at("google")->authorization_url(state));
        res.end();
    });

    CROW_ROUTE(app, "/auth/callback")
    ([this](const crow::request& req, crow::response& res) {
        const char* code_param  = req.url_params.get("code");
        const char* state_param = req.url_params.get("state");

        if (!code_param || !state_param) {
            http_log_.warn("callback", "missing code or state param");
            res.code = 302;
            res.add_header("Location",
                           config_.cors_origin + "/auth-error?reason=missing_params");
            res.end();
            return;
        }

        std::string provider_name;
        if (!consume_state(state_param, provider_name)) {
            // expired or unknown state — covers CSRF attempts and replayed callbacks
            http_log_.warn("callback", "invalid or expired state nonce");
            res.code = 302;
            res.add_header("Location",
                           config_.cors_origin + "/auth-error?reason=invalid_state");
            res.end();
            return;
        }

        const auto pit = providers_.find(provider_name);
        if (pit == providers_.end()) {
            http_log_.error("callback", "unknown provider in state: " + provider_name);
            res.code = 302;
            res.add_header("Location",
                           config_.cors_origin + "/auth-error?reason=unknown_provider");
            res.end();
            return;
        }
        IOAuthProvider* provider = pit->second.get();

        try {
            const auto user_info = provider->exchange_code(code_param);
            const auto player    = auth_service_.find_or_create(
                                       provider->provider_name(), user_info);
            const auto tokens    = auth_service_.issue_tokens(player);

            res.add_header("Set-Cookie", make_access_cookie (tokens.access_token));
            res.add_header("Set-Cookie", make_refresh_cookie(tokens.refresh_token));
            res.code = 302;
            res.add_header("Location", config_.cors_origin);

            http_log_.info("callback",
                           "login: player " + std::to_string(player.id) +
                           " (" + player.username + ")" +
                           " via " + std::string(provider->provider_name()));
        } catch (const std::exception& e) {
            http_log_.error("callback",
                            std::string("oauth exchange failed: ") + e.what());
            res.code = 302;
            res.add_header("Location",
                           config_.cors_origin + "/auth-error?reason=oauth_failed");
        }
        res.end();
    });

    CROW_ROUTE(app, "/auth/refresh").methods(crow::HTTPMethod::Post)
    ([this](const crow::request& req, crow::response& res) {
        const auto refresh_token = read_cookie(req, "refresh_token");
        if (refresh_token.empty()) {
            res.code = 401;
            nlohmann::json j; j["error"] = "UNAUTHORIZED";
            res.set_header("Content-Type", "application/json");
            res.write(j.dump());
            res.end();
            return;
        }

        const auto player_id = auth_service_.validate_refresh_token(refresh_token);
        if (!player_id.has_value()) {
            res.code = 401;
            nlohmann::json j; j["error"] = "TOKEN_EXPIRED";
            res.set_header("Content-Type", "application/json");
            res.write(j.dump());
            res.end();
            return;
        }

        // Confirm account still exists; a deleted account should not refresh indefinitely.
        auto handle = db_pool_.acquire();
        pqxx::work txn(handle.get());
        const auto player = player_repo_.find_by_id(txn, *player_id);
        if (!player.has_value()) {
            res.code = 401;
            nlohmann::json j; j["error"] = "UNAUTHORIZED";
            res.set_header("Content-Type", "application/json");
            res.write(j.dump());
            res.end();
            return;
        }

        const auto tokens = auth_service_.issue_tokens(*player);
        res.add_header("Set-Cookie", make_access_cookie(tokens.access_token));
        res.code = 204;
        res.end();
    });

    CROW_ROUTE(app, "/auth/logout").methods(crow::HTTPMethod::Post)
    ([this](const crow::request&, crow::response& res) {
        // Stateless logout; access token valid until TTL (15 min). Revocation list deferred to Slice 9.
        res.add_header("Set-Cookie", make_clear_cookie("access_token",  "/"));
        res.add_header("Set-Cookie", make_clear_cookie("refresh_token", "/auth/refresh"));
        res.code = 204;
        res.end();
    });
}

template<typename App>
void HttpServer::register_player_routes(App& app)
{
    CROW_ROUTE(app, "/players/me")
    ([this](const crow::request& req) -> crow::response {
        auto auth = require_auth(req, auth_service_, db_pool_, player_repo_);
        if (auto* err = std::get_if<crow::response>(&auth))
            return std::move(*err);
        const auto& player = std::get<Player>(auth);

        nlohmann::json j;
        j["id"]           = player.id;
        j["username"]     = player.username;
        j["games_played"] = player.games_played;

        crow::response res(200, j.dump());
        res.set_header("Content-Type", "application/json");
        return res;
    });
}

template<typename App>
void HttpServer::register_lobby_routes(App& app)
{
    CROW_ROUTE(app, "/lobbies").methods(crow::HTTPMethod::Post)
    ([this](const crow::request& req) -> crow::response {
        auto auth = require_auth(req, auth_service_, db_pool_, player_repo_, api_key_repo_);
        if (auto* err = std::get_if<crow::response>(&auth))
            return std::move(*err);
        const auto& player = std::get<Player>(auth);

        try {
            LobbyMode   mode                 = LobbyMode::UI;
            bool        spawn_bots_on_leave  = false;
            std::string bot_spawn_difficulty = "easy";
            if (!req.body.empty()) {
                try {
                    const auto body = nlohmann::json::parse(req.body);
                    if (body.contains("mode") && body["mode"].is_string())
                        mode = parse_lobby_mode(body["mode"].get<std::string>());
                    if (body.contains("spawn_bots_on_leave") && body["spawn_bots_on_leave"].is_boolean())
                        spawn_bots_on_leave = body["spawn_bots_on_leave"].get<bool>();
                    if (body.contains("bot_spawn_difficulty") && body["bot_spawn_difficulty"].is_string()) {
                        const auto d = body["bot_spawn_difficulty"].get<std::string>();
                        if (d == "easy" || d == "medium" || d == "hard" || d == "random")
                            bot_spawn_difficulty = d;
                    }
                } catch (const std::exception&) {
                    return make_error(400, "MALFORMED_JSON");
                }
            }

            auto handle = db_pool_.acquire();
            pqxx::work txn(handle.get());

            const Lobby lobby = lobby_repo_.create(
                txn, player.id,
                config_.lobby.min_players,
                config_.lobby.max_players,
                mode,
                spawn_bots_on_leave,
                bot_spawn_difficulty
            );

            const int count = lobby_repo_.player_count(txn, lobby.id);
            txn.commit();

            LobbyView lv{ lobby, count };
            crow::response res(201, lobby_view_json(lv).dump());
            res.set_header("Content-Type", "application/json");
            http_log_.info("lobbies", "created lobby " + lobby.id +
                           " code=" + lobby.code +
                           " mode=" + lobby_mode_string(lobby.mode) +
                           " owner=" + std::to_string(player.id));
            return res;
        } catch (const pqxx::unique_violation&) {
            http_log_.warn("lobbies", "owner " + std::to_string(player.id) +
                           " already has an open lobby");
            return make_error(409, "LOBBY_ALREADY_EXISTS");
        } catch (const std::exception& e) {
            http_log_.error("lobbies", std::string("create failed: ") + e.what());
            return make_error(500, "INTERNAL_ERROR");
        }
    });

    CROW_ROUTE(app, "/lobbies")
    ([this](const crow::request& req) -> crow::response {
        auto auth = require_auth(req, auth_service_, db_pool_, player_repo_, api_key_repo_);
        if (auto* err = std::get_if<crow::response>(&auth))
            return std::move(*err);

        try {
            // Optional ?mode=ui|api filter forwarded to list_waiting.
            std::optional<LobbyMode> mode_filter;
            if (const char* mode_param = req.url_params.get("mode")) {
                try { mode_filter = parse_lobby_mode(mode_param); }
                catch (...) { return make_error(400, "INVALID_MODE"); }
            }

            auto handle = db_pool_.acquire();
            pqxx::work txn(handle.get());
            const auto waiting = lobby_repo_.list_waiting(txn, mode_filter);
            const auto active  = lobby_repo_.list_active(txn, std::nullopt);
            txn.commit();

            nlohmann::json j;
            j["lobbies"] = nlohmann::json::array();
            for (const auto& lv : waiting)
                j["lobbies"].push_back(lobby_view_json(lv));
            for (const auto& lv : active)
                j["lobbies"].push_back(lobby_view_json(lv));

            crow::response res(200, j.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        } catch (const std::exception& e) {
            http_log_.error("lobbies", std::string("list failed: ") + e.what());
            return make_error(500, "INTERNAL_ERROR");
        }
    });

    // Lookup by 6-char code — unauthenticated, used by CLI to poll lobby status.
    CROW_ROUTE(app, "/lobbies/<string>")
    ([this](const crow::request&, const std::string& code) -> crow::response {
        try {
            if (code.size() != 6)
                return make_error(404, "NOT_FOUND");

            auto handle = db_pool_.acquire();
            pqxx::work txn(handle.get());
            const auto lobby_opt = lobby_repo_.find_by_code(txn, code);
            if (!lobby_opt)
                return make_error(404, "NOT_FOUND");
            const int count = lobby_repo_.player_count(txn, lobby_opt->id);
            txn.commit();

            crow::response res(200, lobby_view_json(LobbyView{*lobby_opt, count}).dump());
            res.set_header("Content-Type", "application/json");
            return res;
        } catch (const std::exception& e) {
            http_log_.error("lobbies", std::string("get by code failed: ") + e.what());
            return make_error(500, "INTERNAL_ERROR");
        }
    });

    CROW_ROUTE(app, "/lobbies/<string>/join").methods(crow::HTTPMethod::Post)
    ([this](const crow::request& req, const std::string& lobby_id) -> crow::response {
        auto auth = require_auth(req, auth_service_, db_pool_, player_repo_, api_key_repo_);
        if (auto* err = std::get_if<crow::response>(&auth))
            return std::move(*err);
        const auto& player = std::get<Player>(auth);

        try {
            auto handle = db_pool_.acquire();
            pqxx::work txn(handle.get());

            auto lobby_opt = (lobby_id.size() == 36)
                ? lobby_repo_.find_by_id  (txn, lobby_id)
                : lobby_repo_.find_by_code(txn, lobby_id);
            if (!lobby_opt)
                return make_error(404, "LOBBY_NOT_FOUND");

            const Lobby& lobby = *lobby_opt;
            if (lobby.status != LobbyStatus::Waiting)
                return make_error(409, "GAME_ALREADY_STARTED");

            // Reject auth type mismatches: API lobbies require Bearer, UI lobbies require JWT.
            const bool is_bearer = req.get_header_value("Authorization").substr(0, 7) == "Bearer ";
            if (lobby.mode == LobbyMode::API && !is_bearer)
                return make_error(403, "LOBBY_MODE_MISMATCH");
            if (lobby.mode == LobbyMode::UI && is_bearer)
                return make_error(403, "LOBBY_MODE_MISMATCH");

            // AGENT-CTX: add_player is now idempotent for duplicate joins (Slice 7
            // resilience). nullopt means only LOBBY_FULL here — not-waiting is
            // pre-checked above, and duplicates return the existing joined_at.
            const auto joined_at = lobby_repo_.add_player(txn, lobby.id, player.id);
            if (!joined_at)
                return make_error(409, "LOBBY_FULL");

            const int count = lobby_repo_.player_count(txn, lobby.id);
            txn.commit();

            // Publish after commit so subscribers see consistent DB state.
            nlohmann::json ev;
            ev["type"]         = "player_joined";
            ev["lobby_id"]     = lobby.id;
            ev["player_id"]    = player.id;
            ev["username"]     = player.username;
            ev["player_count"] = count;
            ev["joined_at"]    = *joined_at;
            event_bus_.publish("lobby:" + lobby.id, ev.dump());

            http_log_.info("lobbies", "player " + std::to_string(player.id) +
                           " joined lobby " + lobby.id);

            nlohmann::json res_j;
            res_j["lobby_id"] = lobby_id;
            res_j["code"]     = lobby.code;
            crow::response res(200, res_j.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        } catch (const std::exception& e) {
            http_log_.error("lobbies", std::string("join failed: ") + e.what());
            return make_error(500, "INTERNAL_ERROR");
        }
    });

    // transition_status is CAS; false → a concurrent start already won.
    CROW_ROUTE(app, "/lobbies/<string>/start").methods(crow::HTTPMethod::Post)
    ([this](const crow::request& req, const std::string& lobby_id) -> crow::response {
        auto auth = require_auth(req, auth_service_, db_pool_, player_repo_, api_key_repo_);
        if (auto* err = std::get_if<crow::response>(&auth))
            return std::move(*err);
        const auto& player = std::get<Player>(auth);

        try {
            auto handle = db_pool_.acquire();
            pqxx::work txn(handle.get());

            const auto lobby_opt = lobby_repo_.find_by_id(txn, lobby_id);
            if (!lobby_opt)
                return make_error(404, "LOBBY_NOT_FOUND");

            const Lobby& lobby = *lobby_opt;
            if (lobby.creator_id != player.id)
                return make_error(403, "NOT_LOBBY_OWNER");

            if (lobby.status != LobbyStatus::Waiting)
                return make_error(409, "GAME_ALREADY_STARTED");

            const int count = lobby_repo_.player_count(txn, lobby_id);
            if (count + lobby.bot_count < lobby.min_players)
                return make_error(409, "INSUFFICIENT_PLAYERS");

            const bool ok = lobby_repo_.transition_status(
                txn, lobby_id, LobbyStatus::Waiting, LobbyStatus::Starting);
            if (!ok)
                return make_error(409, "GAME_ALREADY_STARTED");

            txn.commit();

            // Publish after commit so WsServer sees updated status in DB.
            nlohmann::json ev;
            ev["type"]     = "lobby_started";
            ev["lobby_id"] = lobby_id;
            ev["code"]     = lobby.code;
            event_bus_.publish("lobby:" + lobby_id, ev.dump());

            // AGENT-CTX: Separate "game:start" channel so WsServer can subscribe
            // once at startup instead of per-lobby. WsServer transitions the lobby
            // Starting→InGame after creating the session (it is the authority on
            // whether the game is actually live; HttpServer only initiates the start).
            nlohmann::json game_ev;
            game_ev["type"]     = "game_start";
            game_ev["lobby_id"] = lobby_id;
            event_bus_.publish("game:start", game_ev.dump());

            http_log_.info("lobbies", "lobby " + lobby_id +
                           " started by player " + std::to_string(player.id));

            nlohmann::json res_j;
            res_j["lobby_id"] = lobby_id;
            res_j["status"]   = "starting";
            crow::response res(200, res_j.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        } catch (const std::exception& e) {
            http_log_.error("lobbies", std::string("start failed: ") + e.what());
            return make_error(500, "INTERNAL_ERROR");
        }
    });

    // Owner-only: toggle spawn_bots_on_leave. Difficulty always fixed to medium.
    CROW_ROUTE(app, "/lobbies/<string>/bot-settings").methods(crow::HTTPMethod::Patch)
    ([this](const crow::request& req, const std::string& lobby_id) -> crow::response {
        auto auth = require_auth(req, auth_service_, db_pool_, player_repo_, api_key_repo_);
        if (auto* err = std::get_if<crow::response>(&auth))
            return std::move(*err);
        const auto& player = std::get<Player>(auth);

        bool spawn_bots = false;
        try {
            const auto body = nlohmann::json::parse(req.body);
            if (!body.contains("spawn_bots_on_leave") || !body["spawn_bots_on_leave"].is_boolean())
                return make_error(400, "MALFORMED_MESSAGE");
            spawn_bots = body["spawn_bots_on_leave"].get<bool>();
        } catch (...) {
            return make_error(400, "MALFORMED_MESSAGE");
        }

        try {
            auto handle = db_pool_.acquire();
            pqxx::work txn(handle.get());

            const auto lobby_opt = lobby_repo_.find_by_id(txn, lobby_id);
            if (!lobby_opt)
                return make_error(404, "LOBBY_NOT_FOUND");
            if (lobby_opt->creator_id != player.id)
                return make_error(403, "NOT_LOBBY_OWNER");
            if (lobby_opt->status != LobbyStatus::Waiting)
                return make_error(409, "GAME_ALREADY_STARTED");

            lobby_repo_.update_bot_settings(txn, lobby_id, spawn_bots, "medium");
            txn.commit();

            nlohmann::json res_j;
            res_j["spawn_bots_on_leave"]  = spawn_bots;
            res_j["bot_spawn_difficulty"] = "medium";
            crow::response res(200, res_j.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        } catch (const std::exception& e) {
            http_log_.error("lobbies", std::string("bot-settings update failed: ") + e.what());
            return make_error(500, "INTERNAL_ERROR");
        }
    });
}

// ---------------------------------------------------------------------------
// State nonce helpers
// ---------------------------------------------------------------------------

std::string HttpServer::generate_state()
{
    unsigned char buf[16];
    // RAND_bytes failure means entropy source is unavailable — throw rather than issue a weak nonce.
    if (RAND_bytes(buf, static_cast<int>(sizeof(buf))) != 1) {
        throw std::runtime_error("OpenSSL RAND_bytes failed — entropy source unavailable");
    }
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (const auto b : buf) oss << std::setw(2) << static_cast<int>(b);
    return oss.str();
}

void HttpServer::add_state(const std::string& nonce, const std::string& provider)
{
    std::lock_guard<std::mutex> lock(states_mu_);
    pending_states_[nonce] = { provider, std::chrono::steady_clock::now() };
}

bool HttpServer::consume_state(const std::string& nonce, std::string& out_provider)
{
    std::lock_guard<std::mutex> lock(states_mu_);

    // Purge-on-lookup bounds map growth to active OAuth flows only.
    const auto now = std::chrono::steady_clock::now();
    for (auto it = pending_states_.begin(); it != pending_states_.end(); ) {
        const auto age_s = std::chrono::duration_cast<std::chrono::seconds>(
                               now - it->second.created_at).count();
        it = (age_s > kStateTtlSeconds) ? pending_states_.erase(it) : ++it;
    }

    const auto it = pending_states_.find(nonce);
    if (it == pending_states_.end()) return false;

    out_provider = std::move(it->second.provider);
    pending_states_.erase(it);
    return true;
}

// ---------------------------------------------------------------------------
// Cookie string builders
// ---------------------------------------------------------------------------

std::string HttpServer::make_access_cookie(const std::string& value) const
{
    std::string s = "access_token=" + value + "; HttpOnly; SameSite=Lax; Path=/";
    if (config_.auth.secure_cookies) s += "; Secure";
    return s;
}

std::string HttpServer::make_refresh_cookie(const std::string& value) const
{
    // Path=/auth/refresh prevents the browser from sending refresh_token to other endpoints.
    std::string s = "refresh_token=" + value +
                    "; HttpOnly; SameSite=Lax; Path=/auth/refresh";
    if (config_.auth.secure_cookies) s += "; Secure";
    return s;
}

std::string HttpServer::make_clear_cookie(const std::string& name,
                                           const std::string& path) const
{
    // Max-Age=0 deletes the cookie immediately; takes precedence over Expires per RFC 6265.
    std::string s = name + "=; Max-Age=0; HttpOnly; SameSite=Lax; Path=" + path;
    if (config_.auth.secure_cookies) s += "; Secure";
    return s;
}

// ---------------------------------------------------------------------------
// Keybinds routes
// ---------------------------------------------------------------------------

template<typename App>
void HttpServer::register_keybinds_routes(App& app)
{
    // GET /players/me/keybinds
    // Returns the authenticated player's saved keybind overrides.
    // Empty array means "use frontend defaults" — the server stores only explicit
    // overrides, never the full default set.
    CROW_ROUTE(app, "/players/me/keybinds")
    ([this](const crow::request& req) -> crow::response {
        auto auth = require_auth(req, auth_service_, db_pool_, player_repo_);
        if (auto* err = std::get_if<crow::response>(&auth))
            return std::move(*err);
        const auto& player = std::get<Player>(auth);

        try {
            auto handle = db_pool_.acquire();
            pqxx::work txn(handle.get());
            const auto binds = keybinds_repo_.get(txn, player.id);
            txn.commit();

            nlohmann::json j = nlohmann::json::array();
            for (const auto& b : binds) {
                nlohmann::json entry;
                entry["action"]    = b.action;
                entry["key_combo"] = b.key_combo;
                j.push_back(entry);
            }

            crow::response res(200, j.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        } catch (const std::exception& e) {
            http_log_.error("keybinds", std::string("get failed: ") + e.what());
            return make_error(500, "INTERNAL_ERROR");
        }
    });

    // PUT /players/me/keybinds
    // Full replace: body is [{action, key_combo}, ...].
    // AGENT-CTX: Full replace (not patch) keeps the server logic trivial — the
    // frontend always sends the complete set of overrides. No merge needed server-side.
    CROW_ROUTE(app, "/players/me/keybinds").methods(crow::HTTPMethod::Put)
    ([this](const crow::request& req) -> crow::response {
        auto auth = require_auth(req, auth_service_, db_pool_, player_repo_);
        if (auto* err = std::get_if<crow::response>(&auth))
            return std::move(*err);
        const auto& player = std::get<Player>(auth);

        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (...) {
            return make_error(400, "MALFORMED_JSON");
        }

        if (!body.is_array()) return make_error(400, "BODY_MUST_BE_ARRAY");

        std::vector<KeyBind> binds;
        binds.reserve(body.size());
        for (const auto& item : body) {
            if (!item.contains("action")    || !item["action"].is_string() ||
                !item.contains("key_combo") || !item["key_combo"].is_string()) {
                return make_error(400, "INVALID_BIND_ENTRY");
            }
            binds.push_back({ item["action"].get<std::string>(),
                               item["key_combo"].get<std::string>() });
        }

        try {
            auto handle = db_pool_.acquire();
            pqxx::work txn(handle.get());
            keybinds_repo_.set(txn, player.id, binds);
            txn.commit();
            return crow::response(204);
        } catch (const std::exception& e) {
            http_log_.error("keybinds", std::string("set failed: ") + e.what());
            return make_error(500, "INTERNAL_ERROR");
        }
    });
}

// ---------------------------------------------------------------------------
// API key routes
// ---------------------------------------------------------------------------

template<typename App>
void HttpServer::register_api_key_routes(App& app)
{
    // POST /players/me/api-keys — generate a new API key (max 1 active per player)
    CROW_ROUTE(app, "/players/me/api-keys").methods(crow::HTTPMethod::Post)
    ([this](const crow::request& req) -> crow::response {
        auto auth = require_auth(req, auth_service_, db_pool_, player_repo_);
        if (auto* err = std::get_if<crow::response>(&auth))
            return std::move(*err);
        const auto& player = std::get<Player>(auth);

        std::string name;
        try {
            const auto body = nlohmann::json::parse(req.body);
            if (!body.contains("name") || !body["name"].is_string())
                return make_error(422, "VALIDATION_ERROR");
            name = body["name"].get<std::string>();
        } catch (...) {
            return make_error(400, "MALFORMED_JSON");
        }

        try {
            auto handle = db_pool_.acquire();
            pqxx::work txn(handle.get());

            std::string err;
            const auto plaintext = api_key_repo_.create(txn, player.id, name, err);
            if (!plaintext) {
                txn.abort();
                if (err == "ACTIVE_KEY_EXISTS")
                    return make_error(409, "ACTIVE_KEY_EXISTS");
                return make_error(422, "VALIDATION_ERROR");
            }
            txn.commit();
            api_keys_cache_.invalidate(player.id);

            // Retrieve the stored record to return expires_at.
            auto handle2 = db_pool_.acquire();
            pqxx::work txn2(handle2.get());
            const auto keys = api_key_repo_.list_for_player(txn2, player.id);
            txn2.commit();

            nlohmann::json j;
            j["key"]  = *plaintext;
            j["name"] = name;
            if (!keys.empty()) {
                j["id"]         = keys[0].id;
                j["expires_at"] = tp_to_iso8601(keys[0].expires_at);
            }

            http_log_.info("api-keys", "created key for player " +
                           std::to_string(player.id) + " name=" + name);

            crow::response res(201, j.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        } catch (const std::exception& e) {
            http_log_.error("api-keys", std::string("create failed: ") + e.what());
            return make_error(500, "INTERNAL_ERROR");
        }
    });

    // GET /players/me/api-keys — list all keys (no plaintext, no hash)
    CROW_ROUTE(app, "/players/me/api-keys")
    ([this](const crow::request& req) -> crow::response {
        auto auth = require_auth(req, auth_service_, db_pool_, player_repo_);
        if (auto* err = std::get_if<crow::response>(&auth))
            return std::move(*err);
        const auto& player = std::get<Player>(auth);

        if (auto cached = api_keys_cache_.get(player.id)) {
            http_log_.info("api-keys", "cache hit api_keys/" + std::to_string(player.id));
            crow::response res(200, *cached);
            res.set_header("Content-Type", "application/json");
            return res;
        }

        try {
            auto handle = db_pool_.acquire();
            pqxx::work txn(handle.get());
            const auto keys = api_key_repo_.list_for_player(txn, player.id);
            txn.commit();

            nlohmann::json arr = nlohmann::json::array();
            for (const auto& k : keys) {
                nlohmann::json entry;
                entry["id"]         = k.id;
                entry["name"]       = k.name;
                entry["created_at"] = tp_to_iso8601(k.created_at);
                entry["expires_at"] = tp_to_iso8601(k.expires_at);
                if (k.revoked_at)
                    entry["revoked_at"] = tp_to_iso8601(*k.revoked_at);
                else
                    entry["revoked_at"] = nullptr;
                arr.push_back(entry);
            }

            const std::string body = arr.dump();
            api_keys_cache_.set(player.id, body);

            crow::response res(200, body);
            res.set_header("Content-Type", "application/json");
            return res;
        } catch (const std::exception& e) {
            http_log_.error("api-keys", std::string("list failed: ") + e.what());
            return make_error(500, "INTERNAL_ERROR");
        }
    });

    // DELETE /players/me/api-keys/<id> — revoke a key by id
    CROW_ROUTE(app, "/players/me/api-keys/<int>").methods(crow::HTTPMethod::Delete)
    ([this](const crow::request& req, int key_id) -> crow::response {
        auto auth = require_auth(req, auth_service_, db_pool_, player_repo_);
        if (auto* err = std::get_if<crow::response>(&auth))
            return std::move(*err);
        const auto& player = std::get<Player>(auth);

        try {
            auto handle = db_pool_.acquire();
            pqxx::work txn(handle.get());
            const bool ok = api_key_repo_.revoke(txn, key_id, player.id);
            if (!ok) {
                txn.abort();
                return make_error(404, "KEY_NOT_FOUND");
            }
            txn.commit();

            api_keys_cache_.invalidate(player.id);
            http_log_.info("api-keys", "revoked key " + std::to_string(key_id) +
                           " for player " + std::to_string(player.id));
            return crow::response(200);
        } catch (const std::exception& e) {
            http_log_.error("api-keys", std::string("revoke failed: ") + e.what());
            return make_error(500, "INTERNAL_ERROR");
        }
    });
}

template<typename App>
void HttpServer::register_examples_routes(App& app)
{
    auto serve_example = [this](const std::string& filename) {
        return [this, filename](const crow::request&) -> crow::response {
            std::ifstream f("examples/" + filename, std::ios::binary);
            if (!f) {
                http_log_.error("examples", "file not found: " + filename);
                return make_error(404, "NOT_FOUND");
            }
            std::ostringstream ss;
            ss << f.rdbuf();
            crow::response res(200, ss.str());
            res.set_header("Content-Type", "application/octet-stream");
            res.set_header("Content-Disposition",
                           "attachment; filename=\"" + filename + "\"");
            return res;
        };
    };

    CROW_ROUTE(app, "/examples/anjeer_template.py")
    (serve_example("anjeer_template.py"));

    CROW_ROUTE(app, "/examples/anjeer_template.cpp")
    (serve_example("anjeer_template.cpp"));
}

// ---------------------------------------------------------------------------
// Spectate token routes
// ---------------------------------------------------------------------------

template<typename App>
void HttpServer::register_spectate_routes(App& app)
{
    // Generates a single-use spectate token tied to the calling player + lobby.
    // API key auth only — browser players have no need for this endpoint.
    CROW_ROUTE(app, "/players/me/spectate-token").methods(crow::HTTPMethod::Post)
    ([this](const crow::request& req) -> crow::response {
        // Reject JWT callers before calling require_auth to return a distinct error.
        const std::string auth_hdr = req.get_header_value("Authorization");
        const bool is_bearer = auth_hdr.size() > 7 && auth_hdr.substr(0, 7) == "Bearer ";
        if (!is_bearer)
            return make_error(403, "JWT_NOT_ALLOWED");

        auto auth = require_auth(req, auth_service_, db_pool_, player_repo_, api_key_repo_);
        if (auto* err = std::get_if<crow::response>(&auth))
            return std::move(*err);
        const auto& player = std::get<Player>(auth);

        std::string lobby_code;
        try {
            const auto body = nlohmann::json::parse(req.body);
            lobby_code = body.value("lobby_code", "");
        } catch (...) {
            return make_error(400, "MALFORMED_JSON");
        }
        if (lobby_code.empty())
            return make_error(400, "MISSING_LOBBY_CODE");

        try {
            auto handle = db_pool_.acquire();
            pqxx::work txn(handle.get());

            if (!lobby_repo_.find_by_code(txn, lobby_code))
                return make_error(404, "LOBBY_NOT_FOUND");

            const auto tok = spectate_token_repo_.create(txn, player.id, lobby_code);
            txn.commit();

            nlohmann::json j;
            j["token"]      = tok.plaintext;
            j["lobby_code"] = lobby_code;
            crow::response res(200, j.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        } catch (const std::exception& e) {
            http_log_.error("spectate-token", std::string("create failed: ") + e.what());
            return make_error(500, "INTERNAL_ERROR");
        }
    });

    // Consumes a spectate token, establishes a full browser session, redirects
    // to the spectator view. Single-use — calling twice returns 401.
    CROW_ROUTE(app, "/auth/spectate")
    ([this](const crow::request& req, crow::response& res) {
        const char* token_param = req.url_params.get("token");
        if (!token_param) {
            res.code = 401;
            nlohmann::json j; j["error"] = "TOKEN_INVALID";
            res.set_header("Content-Type", "application/json");
            res.write(j.dump());
            res.end();
            return;
        }

        try {
            auto handle = db_pool_.acquire();
            pqxx::work txn(handle.get());
            const auto tok = spectate_token_repo_.find_valid_and_consume(txn, token_param);
            if (!tok) {
                txn.commit();
                res.code = 401;
                nlohmann::json j; j["error"] = "TOKEN_INVALID";
                res.set_header("Content-Type", "application/json");
                res.write(j.dump());
                res.end();
                return;
            }

            const auto player = player_repo_.find_by_id(txn, tok->player_id);
            if (!player) {
                txn.commit();
                res.code = 401;
                nlohmann::json j; j["error"] = "TOKEN_INVALID";
                res.set_header("Content-Type", "application/json");
                res.write(j.dump());
                res.end();
                return;
            }
            txn.commit();

            const auto tokens = auth_service_.issue_tokens(*player);
            res.add_header("Set-Cookie", make_access_cookie (tokens.access_token));
            res.add_header("Set-Cookie", make_refresh_cookie(tokens.refresh_token));

            // Trim trailing slash from cors_origin to avoid double-slash in redirect.
            std::string origin = config_.cors_origin;
            while (!origin.empty() && origin.back() == '/') origin.pop_back();

            res.code = 302;
            res.add_header("Location", origin + "/spectate/" + tok->lobby_code);
            res.end();

            http_log_.info("spectate-token",
                           "exchanged token for player " + std::to_string(tok->player_id) +
                           " lobby=" + tok->lobby_code);
        } catch (const std::exception& e) {
            http_log_.error("spectate-token", std::string("exchange failed: ") + e.what());
            res.code = 500;
            nlohmann::json j; j["error"] = "INTERNAL_ERROR";
            res.set_header("Content-Type", "application/json");
            res.write(j.dump());
            res.end();
        }
    });
}

// ---------------------------------------------------------------------------
// Config routes
// ---------------------------------------------------------------------------

// AGENT-CTX: /config/client is intentionally unauthenticated — it exposes only
// non-sensitive game constants the frontend needs before a WS connection is
// established (e.g. max backoff window for the reconnect state machine). Add
// more fields here as needed; never expose secrets.
template<typename App>
void HttpServer::register_config_routes(App& app)
{
    CROW_ROUTE(app, "/config/client")
    ([this](const crow::request&, crow::response& res) {
        nlohmann::json j;
        j["reconnect_window_seconds"] = config_.reconnect.reconnect_window_seconds;
        j["max_queue_size"]           = config_.reconnect.max_queue_size;
        res.set_header("Content-Type", "application/json");
        res.write(j.dump());
        res.end();
    });
}

} // namespace anjeer::server
