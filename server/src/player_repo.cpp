#include "server/player_repo.h"

#include <stdexcept>

namespace anjeer::server {

// Column order in SELECT must match row_to_player().
// SELECT order: id, username, oauth_provider, oauth_id, games_played

Player PlayerRepo::row_to_player(const pqxx::row& row) {
    return Player{
        row[0].as<int64_t>(),      // id
        row[1].as<std::string>(),  // username
        row[2].as<std::string>(),  // oauth_provider
        row[3].as<std::string>(),  // oauth_id
        row[4].as<int>()           // games_played
    };
}

std::optional<Player> PlayerRepo::find_by_oauth(pqxx::transaction_base& txn,
                                                  std::string_view       provider,
                                                  std::string_view       oauth_id) {
    const auto result = txn.exec_params(
        "SELECT id, username, oauth_provider, oauth_id, games_played "
        "FROM players WHERE oauth_provider=$1 AND oauth_id=$2",
        std::string(provider), std::string(oauth_id)
    );
    if (result.empty()) return std::nullopt;
    return row_to_player(result[0]);
}

std::optional<Player> PlayerRepo::find_by_id(pqxx::transaction_base& txn,
                                               int64_t                id) {
    const auto result = txn.exec_params(
        "SELECT id, username, oauth_provider, oauth_id, games_played "
        "FROM players WHERE id=$1",
        id
    );
    if (result.empty()) return std::nullopt;
    return row_to_player(result[0]);
}

Player PlayerRepo::insert(pqxx::transaction_base& txn,
                           std::string_view        username,
                           std::string_view        provider,
                           std::string_view        oauth_id) {
    // pqxx::unique_violation propagates to AuthService on username collision.
    const auto result = txn.exec_params(
        "INSERT INTO players (username, oauth_provider, oauth_id) "
        "VALUES ($1, $2, $3) "
        "RETURNING id, username, oauth_provider, oauth_id, games_played",
        std::string(username), std::string(provider), std::string(oauth_id)
    );
    if (result.empty()) {
        throw std::runtime_error("PlayerRepo::insert: INSERT RETURNING returned no rows");
    }
    return row_to_player(result[0]);
}


} // namespace anjeer::server
