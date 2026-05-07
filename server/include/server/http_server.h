#pragma once

// crow.h is excluded from this header intentionally — Crow's large include chain
// is isolated to http_server.cpp where the App local variable lives.

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "server/api_key_repo.h"
#include "server/auth_service.h"
#include "server/config.h"
#include "server/db.h"
#include "server/event_bus.h"
#include "server/keybinds_repo.h"
#include "server/lobby_repo.h"
#include "server/logger.h"
#include "server/oauth_provider.h"
#include "server/player_repo.h"
#include "server/spectate_token_repo.h"

namespace anjeer::server {

struct HttpServerDeps {
    AuthService&       auth_service;
    PlayerRepo&        player_repo;
    LobbyRepo&         lobby_repo;
    KeybindsRepo&      keybinds_repo;
    ApiKeyRepo&        api_key_repo;
    SpectateTokenRepo& spectate_token_repo;
    IEventBus&         event_bus;
    DbPool&            db_pool;
};

class HttpServer {
public:
    explicit HttpServer(const ServerConfig& config, HttpServerDeps deps);

    void run();

private:
    // Uses OpenSSL RAND_bytes — std::random_device is not sufficient for CSRF nonces.
    static std::string generate_state();

    bool consume_state(const std::string& nonce, std::string& out_provider);
    void add_state    (const std::string& nonce, const std::string& provider);

    // Template avoids pulling crow::App<CORSHandler> into this header.
    template<typename App> void register_auth_routes     (App& app);
    template<typename App> void register_player_routes   (App& app);
    template<typename App> void register_lobby_routes    (App& app);
    template<typename App> void register_keybinds_routes (App& app);
    template<typename App> void register_api_key_routes  (App& app);
    template<typename App> void register_spectate_routes (App& app);
    template<typename App> void register_examples_routes (App& app);

    std::string make_access_cookie (const std::string& value) const;
    std::string make_refresh_cookie(const std::string& value) const;
    std::string make_clear_cookie  (const std::string& name,
                                    const std::string& path) const;

    static constexpr int kStateTtlSeconds = 600;

    const ServerConfig& config_;
    AuthService&        auth_service_;
    PlayerRepo&         player_repo_;
    LobbyRepo&          lobby_repo_;
    KeybindsRepo&       keybinds_repo_;
    ApiKeyRepo&         api_key_repo_;
    SpectateTokenRepo&  spectate_token_repo_;
    IEventBus&          event_bus_;
    DbPool&             db_pool_;
    Logger              http_log_;

    std::unordered_map<std::string, std::unique_ptr<IOAuthProvider>> providers_;

    struct PendingState {
        std::string                           provider;
        std::chrono::steady_clock::time_point created_at;
    };
    std::unordered_map<std::string, PendingState> pending_states_;
    std::mutex                                    states_mu_;
};

} // namespace anjeer::server
