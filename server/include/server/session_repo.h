#pragma once

#include "server/db.h"

#include <nlohmann/json.hpp>
#include <string>

namespace anjeer::server {

class SessionRepo {
public:
    std::string create_session(DbTxn& txn, const std::string& lobby_id);

    std::string create_round(DbTxn& txn, const std::string& session_id,
                              int round_number, const std::string& goal_suit, int pot);

    void end_round  (DbTxn& txn, const std::string& round_id);
    void end_session(DbTxn& txn, const std::string& session_id, int rounds_played);

    void write_error(DbTxn& txn, const std::string& session_id,
                     const std::string& error_type, const std::string& msg,
                     const nlohmann::json& ctx = nlohmann::json::object());
};

} // namespace anjeer::server
