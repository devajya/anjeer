#pragma once

// AGENT-CTX: PlayerRepo is the only path to the players table. No other module
// issues SQL against players directly. This boundary makes the schema change
// surface predictable: a column rename or index addition touches only this file
// and player_repo.cpp.
//
// All methods take pqxx::transaction_base& — they do NOT open transactions.
// Transaction ownership lives in the service layer (AuthService). This ensures
// multi-step operations (find_or_create) are atomic without coupling the repo
// to business logic.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <pqxx/pqxx>

namespace anjeer::server {

struct Player {
    int64_t     id;
    std::string username;
    std::string oauth_provider;
    std::string oauth_id;
    int         games_played;
};

class PlayerRepo {
public:
    PlayerRepo() = default;

    // Returns nullopt if no player with this (provider, oauth_id) pair exists.
    std::optional<Player> find_by_oauth(pqxx::transaction_base& txn,
                                        std::string_view provider,
                                        std::string_view oauth_id);

    // Returns nullopt if no player with this id exists.
    std::optional<Player> find_by_id(pqxx::transaction_base& txn, int64_t id);

    // Returns nullopt if username is already taken (UNIQUE constraint).
    // Caller (AuthService) must resolve username collisions before calling.
    // AGENT-CTX: insert() does NOT enforce UNIQUE itself — it relies on the DB
    // constraint and propagates pqxx::unique_violation to the caller. AuthService
    // retries with a suffixed username. This keeps the uniqueness logic out of SQL.
    Player insert(pqxx::transaction_base& txn,
                  std::string_view username,
                  std::string_view provider,
                  std::string_view oauth_id);

private:
    // AGENT-CTX: Row-to-struct mapping is centralised here so column order
    // changes in the SELECT are caught in one place, not scattered across callers.
    static Player row_to_player(const pqxx::row& row);
};

} // namespace anjeer::server
