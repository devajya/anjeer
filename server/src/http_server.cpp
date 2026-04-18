#include "server/http_server.h"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>

#include <nlohmann/json.hpp>
#include <openssl/rand.h>

// AGENT-CTX: crow.h is included ONLY in this .cpp file. The header deliberately
// keeps Crow out of its include list. Crow is header-only and large (~4000 lines
// across its subheaders); localising it here prevents it from being recompiled
// into every TU that includes http_server.h.
// crow/middlewares/cors.h is NOT included by crow.h — it must be explicitly
// included to get crow::CORSHandler. This is a Crow design choice: middlewares
// are opt-in to avoid pulling in headers that not all users need.
#include <crow.h>
#include <crow/middlewares/cors.h>

namespace anjeer::server {

// ---------------------------------------------------------------------------
// Anonymous-namespace helpers
// ---------------------------------------------------------------------------

namespace {

// AGENT-CTX: read_cookie parses the raw "Cookie: name=val; name2=val2" header.
// We do NOT use Crow's CookieParser middleware context because:
//   (a) accessing middleware context inside a CROW_ROUTE lambda requires
//       passing the typed app reference into the lambda and calling
//       app.get_context<CookieParser>(req), which leaks the Crow type into
//       the header (we explicitly avoid this — see http_server.h comment).
//   (b) the parsing logic here is trivial and avoids the dependency.
// Returns an empty string if the cookie is absent.
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

crow::response unauthorized(const std::string& error_code)
{
    nlohmann::json j;
    j["error"] = error_code;
    crow::response res(401, j.dump());
    res.set_header("Content-Type", "application/json");
    return res;
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
                       DbPool&             db_pool)
    : config_      (config)
    , auth_service_(auth_service)
    , player_repo_ (player_repo)
    , db_pool_     (db_pool)
    , http_log_    ("logs/http_logs.txt")
{
    providers_["github"] = std::make_unique<GitHubOAuthProvider>(config.auth.github);
    providers_["google"] = std::make_unique<GoogleOAuthProvider>(config.auth.google);
}

// ---------------------------------------------------------------------------
// run — constructs Crow app, registers all routes, blocks until shutdown
// ---------------------------------------------------------------------------

void HttpServer::run()
{
    // AGENT-CTX: CORSHandler is the only Crow middleware used. CookieParser
    // is deliberately excluded — see read_cookie() above for the rationale.
    crow::App<crow::CORSHandler> app;

    // AGENT-CTX: allow_credentials() is required so the browser attaches
    // httpOnly cookies on cross-origin requests (frontend :5173 → server :8080).
    // Without it, the browser strips credentials on cross-origin fetch, and
    // the access_token cookie is never sent to /players/me.
    // origin() must be a specific origin (not "*") when allow_credentials() is
    // set — browsers reject "Access-Control-Allow-Origin: *" with credentials.
    auto& cors = app.get_middleware<crow::CORSHandler>();
    cors.global()
        .origin(config_.cors_origin)
        .methods(crow::HTTPMethod::Get,
                 crow::HTTPMethod::Post,
                 crow::HTTPMethod::Options)
        .headers("Content-Type", "Cookie")
        .allow_credentials();

    // =========================================================================
    // Auth routes — GET /auth/<provider>
    // Generates a CSRF-proof state nonce and redirects the browser to the
    // OAuth provider's authorization page.
    // =========================================================================

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

    // =========================================================================
    // Auth route — GET /auth/callback
    // Single unified callback for all OAuth providers. The provider is
    // identified from the state nonce (stored at authorization time), not the
    // URL, so all three providers can share one redirect URI registered in each
    // OAuth app dashboard as http://localhost:8080/auth/callback.
    // =========================================================================

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
            // AGENT-CTX: expired or unknown state. This covers:
            //   - Replayed callbacks (nonce already consumed)
            //   - CSRF attempts with a forged state
            //   - Flows that took > kStateTtlSeconds (10 min)
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

    // =========================================================================
    // POST /auth/refresh
    // Reads the refresh_token httpOnly cookie, validates it, and issues a
    // new access_token cookie. Stateless — no DB lookup for the token itself,
    // only for the player after validation succeeds.
    // =========================================================================

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

        // AGENT-CTX: On refresh we look up the player to confirm the account
        // still exists. A deleted account's refresh token would otherwise
        // issue a new access token indefinitely until the refresh token expires.
        // This is the cost of stateless refresh: one extra DB read per refresh.
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

    // =========================================================================
    // POST /auth/logout
    // Clears both cookies by setting Max-Age=0. No token blacklist — logout
    // is client-side until Slice 6 introduces session tracking.
    // =========================================================================

    CROW_ROUTE(app, "/auth/logout").methods(crow::HTTPMethod::Post)
    ([this](const crow::request&, crow::response& res) {
        // AGENT-CTX: Logout is intentionally stateless (no DB write). The
        // access token remains valid until its TTL expires (default 15 min).
        // A full server-side revocation list is out of scope until Slice 9
        // introduces the API key / session management layer. The practical
        // risk window (15 min) is acceptable for this game's threat model.
        res.add_header("Set-Cookie", make_clear_cookie("access_token",  "/"));
        res.add_header("Set-Cookie", make_clear_cookie("refresh_token", "/auth/refresh"));
        res.code = 204;
        res.end();
    });

    // =========================================================================
    // GET /players/me
    // Returns the authenticated player's profile. Access token validated from
    // the httpOnly cookie before any DB access.
    // =========================================================================

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

    // =========================================================================
    // Start
    // =========================================================================

    app.loglevel(crow::LogLevel::Warning);
    http_log_.info("startup", "listening on :" + std::to_string(config_.http_port));

    app.port(config_.http_port).run();
}

// ---------------------------------------------------------------------------
// State nonce helpers
// ---------------------------------------------------------------------------

std::string HttpServer::generate_state()
{
    unsigned char buf[16];
    // AGENT-CTX: RAND_bytes returns 1 on success, 0 or -1 on failure. Failure
    // means the OS entropy source is unavailable — this is catastrophic and
    // should never happen on a normal Linux/WSL system. Throwing here is the
    // right choice: it surfaces the problem immediately rather than issuing
    // a weak nonce that could be brute-forced to enable CSRF.
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

    // Opportunistic purge of expired entries on every consume call.
    // AGENT-CTX: Purge-on-lookup bounds map growth to at most
    // (concurrent_oauth_flows × kStateTtlSeconds) entries — negligible.
    // A background timer would be more thorough but adds a thread for no
    // meaningful gain at this scale.
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
    // AGENT-CTX: Path=/auth/refresh restricts the browser to sending the
    // refresh_token cookie only to that single endpoint. It is never sent
    // to /players/me, the game WS, or any other route. This minimises
    // exposure of the long-lived token.
    std::string s = "refresh_token=" + value +
                    "; HttpOnly; SameSite=Lax; Path=/auth/refresh";
    if (config_.auth.secure_cookies) s += "; Secure";
    return s;
}

std::string HttpServer::make_clear_cookie(const std::string& name,
                                           const std::string& path) const
{
    // AGENT-CTX: Max-Age=0 instructs the browser to delete the cookie
    // immediately. The Expires=epoch alternative is less reliable on some
    // older browsers; Max-Age takes precedence when both are present (RFC 6265).
    std::string s = name + "=; Max-Age=0; HttpOnly; SameSite=Lax; Path=" + path;
    if (config_.auth.secure_cookies) s += "; Secure";
    return s;
}

} // namespace anjeer::server
