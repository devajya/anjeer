#pragma once

#include "server/config.h"
#include "server/lobby_gateway.h"

namespace anjeer::server {

// Owns the uWebSockets event loop. run() blocks until SIGINT.
class WsServer {
public:
    explicit WsServer(const ServerConfig& cfg, LobbyGateway& lobby_gateway);
    void run();

private:
    ServerConfig  cfg_;
    LobbyGateway& lobby_gateway_;
};

} // namespace anjeer::server
