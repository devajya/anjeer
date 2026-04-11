#pragma once

#include "server/config.h"

namespace anjeer {
namespace server {

// AGENT-CTX: WsServer owns the uWebSockets event loop for the lifetime of the process.
// run() blocks until SIGINT terminates the process. All WebSocket callbacks and the
// heartbeat defer run on the same event-loop thread — no locking needed for the
// connections set. Do not call run() from more than one thread.
class WsServer {
public:
    explicit WsServer(const ServerConfig& cfg);

    // Blocks. Starts listening, heartbeat thread, and runs the uWS event loop.
    // Returns only when the process receives SIGINT/SIGTERM.
    void run();

private:
    ServerConfig cfg_;
};

} // namespace server
} // namespace anjeer
