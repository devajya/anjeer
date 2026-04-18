#pragma once

// AGENT-CTX: http_server.h intentionally does NOT include crow.h.
// The Crow app is a local variable inside HttpServer::run(), so the Crow
// template type (crow::App<...>) never appears in this header. Consumers
// (main.cpp) only construct HttpServer and call run() — they never interact
// with the Crow type directly, keeping Crow's large header out of main.cpp's
// include chain and speeding up incremental builds.

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "server/auth_service.h"
#include "server/config.h"
#include "server/db.h"
#include "server/logger.h"
#include "server/oauth_provider.h"
#include "server/player_repo.h"

namespace anjeer::server {

class HttpServer {
public:
    // AGENT-CTX: HttpServer borrows — does NOT own — AuthService, PlayerRepo,
    // and DbPool. These are constructed in main() and shared with WsServer.
    // Ownership at the call site prevents ordering issues at destruction.
    explicit HttpServer(const ServerConfig& config,
                        AuthService&        auth_service,
                        PlayerRepo&         player_repo,
                        DbPool&             db_pool);

    // Blocks until the server is stopped. Designed to be called from its own
    // std::thread in main(). Crow manages an internal thread pool for request
    // concurrency — the single std::thread here is just the blocking entry point.
    void run();

private:
    // ---------------------------------------------------------------------------
    // State nonce helpers (CSRF protection for OAuth callbacks)
    // ---------------------------------------------------------------------------

    // AGENT-CTX: generate_state() uses OpenSSL RAND_bytes for cryptographic
    // randomness. std::random_device / mt19937 are NOT sufficient for
    // security-critical nonces. OpenSSL is a transitive dep via uSockets, so
    // no new dependency is needed. Produces 32 lowercase hex characters.
    static std::string generate_state();

    // Validates and consumes a nonce in one atomic operation.
    // Also purges entries older than kStateTtlSeconds as a side-effect.
    // Returns true iff nonce was present, unexpired, and is now removed.
    // out_provider is set on true return only.
    // AGENT-CTX: pending_states_ is accessed from Crow's thread pool — all
    // access must hold states_mu_. Map is bounded by TTL purge-on-lookup.
    bool consume_state(const std::string& nonce, std::string& out_provider);
    void add_state    (const std::string& nonce, const std::string& provider);

    // ---------------------------------------------------------------------------
    // Cookie string builders
    // ---------------------------------------------------------------------------

    // AGENT-CTX: SameSite=Lax blocks cross-site POST CSRF while permitting
    // the top-level navigation that completes the OAuth redirect back to our
    // callback URL. SameSite=Strict would break that navigation.
    // The Secure flag is config-driven: dev=false (HTTP), prod=true (nginx TLS).
    // refresh_token uses Path=/auth/refresh so the browser never sends it to
    // /players/me or the game WS — only to the refresh endpoint.
    std::string make_access_cookie (const std::string& value) const;
    std::string make_refresh_cookie(const std::string& value) const;
    std::string make_clear_cookie  (const std::string& name,
                                    const std::string& path) const;

    // ---------------------------------------------------------------------------
    // Constants
    // ---------------------------------------------------------------------------

    // AGENT-CTX: 10 minutes is generous for a browser-based OAuth flow.
    // Shorter values (60s) cause spurious failures on slow connections. Longer
    // values increase the CSRF attack window if a valid nonce leaks. 600s is
    // the industry standard compromise.
    static constexpr int kStateTtlSeconds = 600;

    // ---------------------------------------------------------------------------
    // Data members
    // ---------------------------------------------------------------------------

    const ServerConfig& config_;
    AuthService&        auth_service_;
    PlayerRepo&         player_repo_;
    DbPool&             db_pool_;

    // AGENT-CTX: http_log_ writes to logs/http_logs.txt, separate from
    // server_logs.txt (WS game events) to keep the two concerns readable
    // independently. This mirrors the engine_log / server_log split.
    Logger http_log_;

    // Provider instances are owned here so their lifetime matches HttpServer's.
    // Adding a new provider: implement IOAuthProvider, register in the constructor.
    // HttpServer::run() dispatches via providers_.find(name) — no code change needed.
    std::unordered_map<std::string, std::unique_ptr<IOAuthProvider>> providers_;

    struct PendingState {
        std::string                           provider;
        std::chrono::steady_clock::time_point created_at;
    };
    std::unordered_map<std::string, PendingState> pending_states_;
    std::mutex                                    states_mu_;
};

} // namespace anjeer::server
