#pragma once

#include "server/db.h"
#include "server/event_bus.h"
#include "server/game_session.h"  // WsHandle, PerSocketData
#include "server/lobby_repo.h"
#include "server/logger.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace anjeer::server {

// Encapsulates all lobby-subscription state and the three operations WsServer needs.
// All public methods must be called on the uWS event-loop thread.
class LobbyGateway {
public:
    LobbyGateway(DbPool& db_pool, LobbyRepo& lobby_repo, IEventBus& event_bus);

    // Synchronous DB query — accepted for Slice 6.
    void handle_subscribe  (WsHandle ws, const std::string& lobby_id,
                            uWS::Loop* loop, Logger& log);
    void handle_unsubscribe(WsHandle ws);
    void cleanup           (WsHandle ws);

private:
    DbPool&    db_pool_;
    LobbyRepo& lobby_repo_;
    IEventBus& event_bus_;

    struct Sub { std::string lobby_id; uint64_t bus_sub_id; };
    std::unordered_map<WsHandle, Sub> subs_;
};

} // namespace anjeer::server
