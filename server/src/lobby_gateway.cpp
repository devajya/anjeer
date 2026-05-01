#include "server/lobby_gateway.h"

#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

namespace anjeer::server {

LobbyGateway::LobbyGateway(DbPool& db_pool, LobbyRepo& lobby_repo, IEventBus& event_bus)
    : db_pool_(db_pool)
    , lobby_repo_(lobby_repo)
    , event_bus_(event_bus)
{}

void LobbyGateway::cleanup(WsHandle ws) {
    auto it = subs_.find(ws);
    if (it == subs_.end()) return;
    event_bus_.unsubscribe(it->second.bus_sub_id);
    subs_.erase(it);
}

void LobbyGateway::handle_unsubscribe(WsHandle ws) {
    cleanup(ws);
}

void LobbyGateway::handle_subscribe(WsHandle ws, const std::string& lobby_id,
                                     uWS::Loop* loop, Logger& log) {
    if (lobby_id.empty()) {
        nlohmann::json err{{"type","error"},{"code","MALFORMED_MESSAGE"},
                           {"message","subscribe_lobby requires lobby_id"}};
        ws->send(err.dump(), uWS::OpCode::TEXT);
        return;
    }

    cleanup(ws);

    std::optional<Lobby>     opt_lobby;
    std::vector<LobbyPlayer> players;
    try {
        auto handle = db_pool_.acquire();
        pqxx::work txn(handle.get());
        opt_lobby = lobby_repo_.find_by_id(txn, lobby_id);
        if (opt_lobby) players = lobby_repo_.list_players(txn, lobby_id);
        txn.commit();
    } catch (const std::exception& ex) {
        log.error("subscribe_lobby", std::string("DB error: ") + ex.what());
        nlohmann::json err{{"type","error"},{"code","INTERNAL_ERROR"},
                           {"message","Internal server error"}};
        ws->send(err.dump(), uWS::OpCode::TEXT);
        return;
    }

    if (!opt_lobby) {
        nlohmann::json err{{"type","error"},{"code","LOBBY_NOT_FOUND"},
                           {"message","Lobby not found"}};
        ws->send(err.dump(), uWS::OpCode::TEXT);
        return;
    }

    const Lobby& lobby = *opt_lobby;
    nlohmann::json players_arr = nlohmann::json::array();
    for (const auto& p : players) {
        players_arr.push_back({
            {"player_id", p.player_id},
            {"username",  p.username},
            {"joined_at", p.joined_at},
        });
    }
    nlohmann::json snap{
        {"type",        "lobby_state"},
        {"lobby_id",    lobby.id},
        {"code",        lobby.code},
        {"creator_id",  lobby.creator_id},
        {"status",      lobby_status_string(lobby.status)},
        {"min_players", lobby.min_players},
        {"max_players", lobby.max_players},
        {"players",     players_arr},
    };
    ws->send(snap.dump(), uWS::OpCode::TEXT);
    log.info("subscribe_lobby", "sent lobby_state to ws for lobby " + lobby_id);

    // Handler fires on the LocalEventBus publish thread (Crow); loop->defer()
    // marshals ws->send() back to the uWS event-loop thread.
    uint64_t sub_id = event_bus_.subscribe(
        "lobby:" + lobby_id,
        [ws, loop, &subs = subs_](const std::string& payload) {
            loop->defer([ws, payload, &subs]() {
                if (subs.count(ws)) {
                    ws->send(payload, uWS::OpCode::TEXT);
                }
            });
        }
    );
    subs_[ws] = Sub{lobby_id, sub_id};
    log.info("subscribe_lobby", "subscribed ws to bus channel lobby:" + lobby_id);
}

} // namespace anjeer::server
