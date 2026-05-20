// AGENT-CTX: This file exists solely to break a dependency-chain problem.
// BotManager::get_displaceable_bot_slot(const GameSession&) calls
// session.slot_balances(), which is defined in game_session.cpp.
// bot_manager_tests is a pure unit-test target that doesn't link
// game_session.cpp (or its heavy transitive deps: uWS, DB, engine, etc.).
// Putting the GameSession overload here lets the test target compile
// bot_manager.cpp cleanly while the server binary and ws_server_tests
// both include this file and therefore do get the GameSession bridge.

#include "server/bot_manager.h"
#include "server/game_session.h"

namespace anjeer::server {

int BotManager::get_displaceable_bot_slot(const std::string& lobby_id,
                                          const GameSession& session) const {
    return get_displaceable_bot_slot(lobby_id, session.slot_balances());
}

} // namespace anjeer::server
