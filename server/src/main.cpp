#include "server/config.h"
#include "server/ws_server.h"

#include <iostream>
#include <string>

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

    anjeer::server::ServerConfig cfg;
    try {
        cfg = anjeer::server::load_config(config_path);
    } catch (const std::exception& e) {
        std::cerr << "[fatal] " << e.what() << "\n";
        return 1;
    }

    std::cout << "[server] loaded config: " << config_path << "\n";

    anjeer::server::WsServer srv(cfg);
    srv.run();

    return 0;
}
