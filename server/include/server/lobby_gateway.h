#pragma once

#include "server/db.h"
#include "server/event_bus.h"
#include "server/ws_types.h"  // WsHandle, PerSocketData
#include "server/lobby_repo.h"
#include "server/logger.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace anjeer::server {

struct BotPlayerEntry {
    std::string bot_uuid;
    std::string username;
    std::string difficulty;  // "easy" | "medium" | "hard"
};

// Encapsulates all lobby-subscription state and the three operations WsServer needs.
// All public methods must be called on the uWS event-loop thread.
class LobbyGateway {
public:
    LobbyGateway(DbPool& db_pool, LobbyRepo& lobby_repo, IEventBus& event_bus);

    // Synchronous DB query — accepted for Slice 6.
    // bots: in-memory bot entries from BotManager to merge into the player list.
    void handle_subscribe  (WsHandle ws, const std::string& lobby_id,
                            uWS::Loop* loop, Logger& log,
                            const std::vector<BotPlayerEntry>& bots = {});
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
