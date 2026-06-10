#include "server/api_key_repo.h"
#include "server/auth_service.h"
#include "server/bot_manager.h"
#include "server/bot_scheduler.h"
#include "server/config.h"
#include "server/db.h"
#include "server/event_bus.h"
#include "server/http_server.h"
#include "server/keybinds_repo.h"
#include "server/lobby_gateway.h"
#include "server/lobby_repo.h"
#include "server/player_repo.h"
#include "server/spectate_token_repo.h"
#include "server/ws_server.h"

#include <iostream>
#include <string>
#include <thread>

#include <pqxx/pqxx>

static std::string parse_config_path(int argc, char* argv[]) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--config") {
            return argv[i + 1];
        }
    }
    return "config/default.json";
}

int main(int argc, char* argv[]) {
    const std::string config_path = parse_config_path(argc, argv);

    static anjeer::server::ServerConfig cfg;
    try {
        cfg = anjeer::server::load_config(config_path);
    } catch (const std::exception& e) {
        std::cerr << "[fatal] " << e.what() << "\n";
        return 1;
    }

    std::cout << "[server] loaded config: " << config_path << "\n";

    // Migrations before pool open; failure here is fatal to avoid serving against a stale schema.
    try {
        pqxx::connection migration_conn(cfg.db.connection_string);
        anjeer::server::DbMigrator migrator(migration_conn, cfg.db.migrations_dir);
        migrator.run();
        std::cout << "[server] DB migrations applied\n";
    } catch (const std::exception& e) {
        std::cerr << "[fatal] DB migration failed: " << e.what() << "\n";
        return 1;
    }

    // Static storage: all objects outlive any thread that holds references to them.
    static anjeer::server::DbPool              db_pool(cfg.db.connection_string, cfg.db.pool_size);
    static anjeer::server::PlayerRepo          player_repo;
    static anjeer::server::LobbyRepo           lobby_repo;
    static anjeer::server::KeybindsRepo        keybinds_repo;
    static anjeer::server::ApiKeyRepo          api_key_repo;
    static anjeer::server::SpectateTokenRepo   spectate_token_repo(cfg.auth.spectate_token_ttl_minutes);
    static anjeer::server::LocalEventBus       event_bus;  // Slice 16: swap for RedisEventBus
    static anjeer::server::BotScheduler        bot_scheduler(cfg.bots.scheduler_threads);
    static anjeer::server::BotManager          bot_manager(bot_scheduler, cfg.bots, cfg.game.hard);
    static anjeer::server::LobbyGateway        lobby_gateway(db_pool, lobby_repo, event_bus);
    static anjeer::server::AuthService         auth_service(db_pool, player_repo, cfg);
    static anjeer::server::HttpServer          http_server(cfg, anjeer::server::HttpServerDeps{
                                                               auth_service, player_repo,
                                                               lobby_repo, keybinds_repo,
                                                               api_key_repo, spectate_token_repo,
                                                               event_bus, db_pool});

    std::thread http_thread([&http_server] { http_server.run(); });
    http_thread.detach();

    anjeer::server::WsServer ws_server(cfg, anjeer::server::WsServerDeps{
                                           lobby_gateway, db_pool, lobby_repo,
                                           auth_service, api_key_repo, event_bus,
                                           bot_manager});
    ws_server.run();

    return 0;
}
