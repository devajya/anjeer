#pragma once

// AGENT-CTX: ApiKeyRepo is the only path to the api_keys table. All hashing
// is internal — callers pass plaintext keys; the hash never escapes this module
// except when stored in the DB. find_valid_by_hash() is the hot path called on
// every WS upgrade for API-key-authenticated connections.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <pqxx/pqxx>

#include "server/db.h"

namespace anjeer::server {

// Safe for API responses — key_hash is never included.
struct ApiKeyView {
    int32_t     id;
    int32_t     player_id;
    std::string name;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point expires_at;
    std::optional<std::chrono::system_clock::time_point> revoked_at;
};

// Internal only — used by find_valid_by_hash for the upgrade handler.
struct ApiKeyRecord : ApiKeyView {
    std::string key_hash;
};

class ApiKeyRepo {
public:
    ApiKeyRepo() = default;

    // Generates a new API key, stores its SHA-256 hash, returns the plaintext.
    // Returns nullopt and sets err if:
    //   - player already has a non-revoked key  (err = "ACTIVE_KEY_EXISTS")
    //   - name is empty or > 100 chars          (err = "VALIDATION_ERROR")
    // The returned plaintext starts with "ank_" and is shown exactly once.
    std::optional<std::string> create(DbTxn& txn,
                                      int32_t player_id,
                                      const std::string& name,
                                      std::string& err);

    // Looks up by pre-computed SHA-256 hex hash. Returns nullopt if:
    //   - no row with that hash
    //   - row is revoked (revoked_at IS NOT NULL)
    //   - row is expired (expires_at <= NOW())
    // Callers compute the hash via sha256_hex() before calling.
    std::optional<ApiKeyRecord> find_valid_by_hash(DbTxn& txn,
                                                    const std::string& key_hash);

    // Returns all keys for player ordered newest-first. key_hash excluded.
    std::vector<ApiKeyView> list_for_player(DbTxn& txn, int32_t player_id);

    // Marks revoked_at = NOW(). Returns false if key_id not found or not owned
    // by player_id (ownership check prevents cross-player revocation).
    bool revoke(DbTxn& txn, int32_t key_id, int32_t player_id);

private:
    static ApiKeyView   row_to_view  (const pqxx::row& row);
    static ApiKeyRecord row_to_record(const pqxx::row& row);
    static std::chrono::system_clock::time_point parse_timestamp(const std::string& s);
};

} // namespace anjeer::server
