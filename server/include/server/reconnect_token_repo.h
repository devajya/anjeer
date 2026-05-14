#pragma once

// AGENT-CTX: ReconnectTokenRepo is the only path to the reconnect_tokens table.
// Tokens are issued per game slot on join, stored hashed (SHA-256), and deleted on
// successful reattach or expiry. The plaintext token is sent to the client once and
// never stored server-side — only sha256_hex(token) is persisted.
//
// Unlike ApiKeyRepo (which is called from Crow HTTP handlers and takes a caller-owned
// DbTxn), ReconnectTokenRepo owns its DbPool reference and acquires connections
// internally. This matches the WsServer usage pattern: the WS upgrade handler needs
// a simple synchronous validate() call with no surrounding transaction context.
//
// validate() is called on the event-loop thread (synchronous). The call is ~1 DB
// round-trip (~1 ms on local hardware) — acceptable for connection establishment.
// Flagged for async migration alongside the crypto_util note in Slice 16.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "server/db.h"

namespace anjeer::server {

struct ReconnectTokenRecord {
    std::string  token_hash;
    int64_t      player_id;
    std::string  lobby_id;   // UUID string
    std::optional<std::string> session_id;  // nullable — hook for crash recovery
    std::chrono::system_clock::time_point expires_at;
};

class ReconnectTokenRepo {
public:
    explicit ReconnectTokenRepo(DbPool& pool);

    // Generates a new rtk_<64hex> token, stores its SHA-256 hash, returns the
    // plaintext. The row expires at NOW() + window_seconds.
    // Any prior token for the same (player_id, lobby_id) pair is replaced.
    std::string create(int64_t player_id,
                       const std::string& lobby_id,
                       int window_seconds);

    // Validates plaintext token: hashes it and looks up the DB row.
    // Returns the record if the row exists and has not expired; nullopt otherwise.
    // Does NOT delete the row — callers must call revoke() on successful reattach.
    std::optional<ReconnectTokenRecord> validate(const std::string& token_plaintext);

    // Deletes the row identified by hash. Called on successful reattach and on
    // expiry cleanup. No-op if the row does not exist.
    void revoke(const std::string& token_hash);

    // Deletes all expired rows. Call periodically from a WsServer idle timer.
    void cleanup_expired();

private:
    DbPool& pool_;

    static std::chrono::system_clock::time_point parse_ts(const std::string& s);
};

} // namespace anjeer::server
