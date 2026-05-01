#include "server/session_repo.h"

namespace anjeer::server {

std::string SessionRepo::create_session(DbTxn& txn, const std::string& lobby_id) {
    const auto r = txn.exec_params(
        "INSERT INTO game_sessions (lobby_id) VALUES ($1) RETURNING id",
        lobby_id
    );
    return r[0][0].as<std::string>();
}

std::string SessionRepo::create_round(DbTxn& txn, const std::string& session_id,
                                       int round_number, const std::string& goal_suit,
                                       int pot) {
    const auto r = txn.exec_params(
        "INSERT INTO rounds (session_id, round_number, goal_suit, pot) "
        "VALUES ($1, $2, $3, $4) RETURNING id",
        session_id, round_number, goal_suit, pot
    );
    return r[0][0].as<std::string>();
}

void SessionRepo::end_round(DbTxn& txn, const std::string& round_id) {
    txn.exec_params(
        "UPDATE rounds SET ended_at = NOW() WHERE id = $1",
        round_id
    );
}

void SessionRepo::end_session(DbTxn& txn, const std::string& session_id,
                               int rounds_played) {
    txn.exec_params(
        "UPDATE game_sessions "
        "SET status = 'ended', rounds_played = $2, ended_at = NOW() "
        "WHERE id = $1",
        session_id, rounds_played
    );
}

void SessionRepo::write_error(DbTxn& txn, const std::string& session_id,
                               const std::string& error_type, const std::string& msg,
                               const nlohmann::json& ctx) {
    txn.exec_params(
        "INSERT INTO session_errors (session_id, error_type, message, context_json) "
        "VALUES ($1, $2, $3, $4)",
        session_id, error_type, msg, ctx.dump()
    );
}

} // namespace anjeer::server
