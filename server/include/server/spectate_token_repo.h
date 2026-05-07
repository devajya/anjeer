#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "server/db.h"

namespace anjeer::server {

struct SpectateToken {
    int64_t     id;
    std::string token_hash;   // SHA-256 hex — stored in DB, never returned to clients
    std::string plaintext;    // populated by create() only; empty when loaded from DB
    int64_t     player_id;
    std::string lobby_code;
    std::chrono::system_clock::time_point expires_at;
    std::optional<std::chrono::system_clock::time_point> used_at;
};

// Single-use, short-lived tokens that allow a terminal player to open a
// spectator view in the browser without requiring an active browser session.
// Pattern mirrors ApiKeyRepo: plaintext is never stored — only sha256_hex(token).
class SpectateTokenRepo {
public:
    explicit SpectateTokenRepo(int ttl_minutes);

    // Generates a new stk_ token, stores its hash, returns the plaintext.
    SpectateToken create(DbTxn& txn,
                         int64_t player_id,
                         const std::string& lobby_code);

    // Returns the token and marks it used atomically. Returns nullopt if:
    //   - hash not found
    //   - expires_at <= NOW()
    //   - used_at IS NOT NULL (already consumed)
    std::optional<SpectateToken> find_valid_and_consume(DbTxn& txn,
                                                         const std::string& raw_token);

private:
    int ttl_minutes_;

    static SpectateToken row_to_token(const pqxx::row& row);
    static std::chrono::system_clock::time_point parse_ts(const std::string& s);
};

} // namespace anjeer::server
