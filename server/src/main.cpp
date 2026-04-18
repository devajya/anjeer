#include "server/auth_service.h"
#include "server/config.h"
#include "server/db.h"
#include "server/http_server.h"
#include "server/player_repo.h"
#include "server/ws_server.h"

#include <iostream>
#include <string>
#include <thread>

#include <pqxx/pqxx>

// AGENT-CTX: Config path is the only runtime argument. All other behaviour is driven
// by the config file. This keeps the binary interface minimal and consistent with the
// test harness pattern (tests pass --config tests/test.json to override defaults).
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

    // AGENT-CTX: Migrations run before the connection pool is opened and before
    // either server starts accepting connections. A dedicated non-pool connection
    // is used so DbMigrator does not consume a pool slot during startup.
    // Failure here is fatal: serving requests against a stale schema causes
    // silent data corruption (e.g. missing columns). The process exits rather
    // than continuing in a broken state.
    try {
        pqxx::connection migration_conn(cfg.db.connection_string);
        anjeer::server::DbMigrator migrator(migration_conn, cfg.db.migrations_dir);
        migrator.run();
        std::cout << "[server] DB migrations applied\n";
    } catch (const std::exception& e) {
        std::cerr << "[fatal] DB migration failed: " << e.what() << "\n";
        return 1;
    }

    // Static storage duration: HttpServer and AuthService store const ServerConfig&
    // and other references; http_thread is detached with no join point while
    // ws_server.run() is blocking. Static locals are destroyed at process exit
    // after all threads are killed — no dangling references regardless of whether
    // ws_server.run() ever returns. Slice 6 replaces the detach with a shutdown
    // channel so both servers can be stopped and joined cleanly.
    static anjeer::server::DbPool      db_pool(cfg.db.connection_string, cfg.db.pool_size);
    static anjeer::server::PlayerRepo  player_repo;
    static anjeer::server::AuthService auth_service(db_pool, player_repo, cfg);
    static anjeer::server::HttpServer  http_server(cfg, auth_service, player_repo, db_pool);

    std::thread http_thread([&http_server] { http_server.run(); });
    http_thread.detach();

    // WS game server — blocks on the uWS event loop until process exit.
    anjeer::server::WsServer ws_server(cfg);
    ws_server.run();

    return 0;
}
