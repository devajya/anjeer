#pragma once

#include "server/api_key_repo.h"
#include "server/auth_service.h"
#include "server/config.h"
#include "server/db.h"
#include "server/event_bus.h"
#include "server/game_session.h"
#include "server/lobby_gateway.h"
#include "server/lobby_repo.h"
#include "server/logger.h"
#include "server/rate_limiter.h"
#include "server/session_queue.h"
#include "server/session_repo.h"
#include "server/ws_types.h"

#include <readerwriterqueue.h>

#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace anjeer::server {

struct WsServerDeps {
    LobbyGateway& lobby_gateway;
    DbPool&       db_pool;
    LobbyRepo&    lobby_repo;
    AuthService&  auth_service;
    ApiKeyRepo&   api_key_repo;
    IEventBus&    event_bus;
};

class WsServer {
public:
    explicit WsServer(const ServerConfig& cfg, WsServerDeps deps);
    void run();

private:
    // AGENT-CTX: ActiveSession is owned entirely by WsServer and accessed only
    // on the uWS event-loop thread (timer drain, .open, .close, defer callbacks).
    // GameSession's game-loop thread only accesses inbound/outbound queues (SPSC,
    // lock-free). No mutex is needed anywhere in WsServer's state because uWS
    // guarantees single-threaded access to all event callbacks and loop->defer().
    struct ActiveSession {
        std::unique_ptr<moodycamel::ReaderWriterQueue<NetEvent>>  inbound;
        std::unique_ptr<moodycamel::ReaderWriterQueue<GameEvent>> outbound;
        std::unique_ptr<GameSession>                               session;
        std::vector<SlotInfo>                                      slots_;        // authoritative slot list
        std::unordered_map<int32_t, WsHandle>                      slot_to_ws_;
        std::unordered_map<WsHandle, int32_t>                      ws_to_slot_;
        LobbyMode                                                  lobby_mode_    = LobbyMode::UI;
        // Spectators are tracked separately from player slots; spectator_id is
        // player_id (may be -1 for unauthenticated). ws_to_spectator_ enables
        // O(1) cleanup in .close without scanning the forward map.
        std::unordered_map<int32_t, WsHandle>                      spectator_handles_;
        std::unordered_map<WsHandle, int32_t>                      ws_to_spectator_;
        int32_t                                                    spectator_count_ = 0;
    };

    void create_session      (const std::string& lobby_id);
    void teardown_session    (const std::string& lobby_id);
    void drain_all_on_loop   ();
    void handle_leave_lobby  (WsHandle ws, const std::string& lobby_id);
    void handle_spectate_lobby(WsHandle ws, const std::string& lobby_id);

    // ── Dependencies ─────────────────────────────────────────────────────────
    ServerConfig   cfg_;
    LobbyGateway&  lobby_gateway_;
    DbPool&        db_pool_;
    LobbyRepo&     lobby_repo_;
    AuthService&   auth_service_;
    ApiKeyRepo&    api_key_repo_;
    IEventBus&     event_bus_;
    RateLimiter    rate_limiter_;

    // AGENT-CTX: Loggers are members (not locals in run()) so that create_session,
    // teardown_session, and drain_all_on_loop can reference them without passing
    // them through every call site. All log writes happen on the uWS loop thread
    // (or inside GameSession on its own thread, which has its own Logger refs).
    Logger server_log_;
    Logger engine_log_;
    Logger frontend_log_;

    SessionRepo  session_repo_;
    std::mt19937 rng_;

    // AGENT-CTX: Both maps are only ever accessed on the uWS event-loop thread.
    // player_to_lobby_ provides O(1) reconnect routing without a DB query.
    std::unordered_map<std::string, ActiveSession> active_sessions_;
    std::unordered_map<int64_t, std::string>       player_to_lobby_;

    uint64_t game_start_sub_id_ = 0;
};

} // namespace anjeer::server
