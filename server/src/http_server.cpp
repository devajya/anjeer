#include "server/http_server.h"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>

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
    j["id"]           = lv.lobby.id;
    j["code"]         = lv.lobby.code;
    j["creator_id"]   = lv.lobby.creator_id;
    j["status"]       = lobby_status_string(lv.lobby.status);
    j["min_players"]  = lv.lobby.min_players;
    j["max_players"]  = lv.lobby.max_players;
    j["player_count"] = lv.player_count;
    j["created_at"]   = lv.lobby.created_at;
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

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

HttpServer::HttpServer(const ServerConfig& config,
                       AuthService&        auth_service,
                       PlayerRepo&         player_repo,
                       LobbyRepo&          lobby_repo,
                       KeybindsRepo&       keybinds_repo,
                       IEventBus&          event_bus,
                       DbPool&             db_pool)
    : config_        (config)
    , auth_service_  (auth_service)
    , player_repo_   (player_repo)
    , lobby_repo_    (lobby_repo)
    , keybinds_repo_ (keybinds_repo)
    , event_bus_     (event_bus)
    , db_pool_       (db_pool)
    , http_log_      ("logs/http_logs.txt")
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
                 crow::HTTPMethod::Options)
        .headers("Content-Type", "Cookie")
        .allow_credentials();

    register_auth_routes(app);
    register_player_routes(app);
    register_lobby_routes(app);
    register_keybinds_routes(app);

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
        auto auth = require_auth(req, auth_service_, db_pool_, player_repo_);
        if (auto* err = std::get_if<crow::response>(&auth))
            return std::move(*err);
        const auto& player = std::get<Player>(auth);

        try {
            auto handle = db_pool_.acquire();
            pqxx::work txn(handle.get());

            const Lobby lobby = lobby_repo_.create(
                txn, player.id,
                config_.lobby.min_players,
                config_.lobby.max_players
            );

            const int count = lobby_repo_.player_count(txn, lobby.id);
            txn.commit();

            LobbyView lv{ lobby, count };
            crow::response res(201, lobby_view_json(lv).dump());
            res.set_header("Content-Type", "application/json");
            http_log_.info("lobbies", "created lobby " + lobby.id +
                           " code=" + lobby.code +
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
        auto auth = require_auth(req, auth_service_, db_pool_, player_repo_);
        if (auto* err = std::get_if<crow::response>(&auth))
            return std::move(*err);

        try {
            auto handle = db_pool_.acquire();
            pqxx::work txn(handle.get());
            const auto views = lobby_repo_.list_waiting(txn);
            txn.commit();

            nlohmann::json j;
            j["lobbies"] = nlohmann::json::array();
            for (const auto& lv : views)
                j["lobbies"].push_back(lobby_view_json(lv));

            crow::response res(200, j.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        } catch (const std::exception& e) {
            http_log_.error("lobbies", std::string("list failed: ") + e.what());
            return make_error(500, "INTERNAL_ERROR");
        }
    });

    CROW_ROUTE(app, "/lobbies/<string>/join").methods(crow::HTTPMethod::Post)
    ([this](const crow::request& req, const std::string& lobby_id) -> crow::response {
        auto auth = require_auth(req, auth_service_, db_pool_, player_repo_);
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
            if (lobby.status != LobbyStatus::Waiting)
                return make_error(409, "GAME_ALREADY_STARTED");

            // AGENT-CTX: add_player is now idempotent for duplicate joins (Slice 7
            // resilience). nullopt means only LOBBY_FULL here — not-waiting is
            // pre-checked above, and duplicates return the existing joined_at.
            const auto joined_at = lobby_repo_.add_player(txn, lobby_id, player.id);
            if (!joined_at)
                return make_error(409, "LOBBY_FULL");

            const int count = lobby_repo_.player_count(txn, lobby_id);
            txn.commit();

            // Publish after commit so subscribers see consistent DB state.
            nlohmann::json ev;
            ev["type"]         = "player_joined";
            ev["lobby_id"]     = lobby_id;
            ev["player_id"]    = player.id;
            ev["username"]     = player.username;
            ev["player_count"] = count;
            ev["joined_at"]    = *joined_at;
            event_bus_.publish("lobby:" + lobby_id, ev.dump());

            http_log_.info("lobbies", "player " + std::to_string(player.id) +
                           " joined lobby " + lobby_id);

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
        auto auth = require_auth(req, auth_service_, db_pool_, player_repo_);
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
            if (count < lobby.min_players)
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

} // namespace anjeer::server
