#pragma once

// AGENT-CTX: AuthService is the single authority for player identity in the
// HTTP layer. It owns:
//   - find_or_create: idempotent OAuth login → Player row
//   - JWT issuance and validation (delegated to JwtService)
//
// Transaction ownership: find_or_create opens and commits its own pqxx::work.
// PlayerRepo methods receive that transaction — they never open their own.
// This ensures the find → probe-username → insert sequence is atomic.
//
// Slice 6 seam: AuthService does NOT touch WS slots or lobby membership.
// The handoff from HTTP login → WS session is wired in HttpServer (Crow)
// and WsServer respectively. AuthService only asserts identity.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "server/config.h"
#include "server/db.h"
#include "server/oauth_provider.h"
#include "server/player_repo.h"

namespace anjeer::server {

// AGENT-CTX: JwtService is extracted from AuthService so unit tests can
// exercise JWT signing/validation without constructing a DbPool.
// All tokens are HMAC-SHA256 ("hs256"). The "type" claim ("access" or
// "refresh") prevents a refresh token from being used as an access token
// and vice versa — this is the only binding enforced beyond expiry.
// jwt_secret must be ≥ 32 bytes in production; smaller secrets are
// accepted by jwt-cpp but are cryptographically weak. The config loader
// does not enforce this constraint — document it in config/prod.json.
struct JwtService {
    struct TokenPair {
        std::string access_token;
        std::string refresh_token;
    };

    explicit JwtService(std::string_view secret,
                         int             access_ttl_seconds,
                         int             refresh_ttl_seconds);

    TokenPair              issue          (const Player& player) const;
    // Returns the player id if the token is valid, not expired, and has
    // type=="access". Returns nullopt on any failure (expired, bad sig,
    // wrong type, malformed).
    std::optional<int64_t> validate_access (std::string_view token) const;
    // Same contract as validate_access but checks type=="refresh".
    std::optional<int64_t> validate_refresh(std::string_view token) const;

private:
    std::optional<int64_t> validate_token(std::string_view token,
                                           std::string_view expected_type) const;
    std::string secret_;
    int         access_ttl_;
    int         refresh_ttl_;
};

class AuthService {
public:
    explicit AuthService(DbPool& pool, PlayerRepo& repo, const ServerConfig& config);

    // Idempotent: returns existing player if (provider, oauth_id) already
    // exists; creates a new player with starting_balance otherwise.
    // Sanitizes the provider username before storing (lowercase, spaces→_).
    // Resolves username collisions by appending _2, _3, ...
    Player find_or_create(std::string_view provider, const OAuthUserInfo& info);

    JwtService::TokenPair  issue_tokens         (const Player& player);
    std::optional<int64_t> validate_access_token (std::string_view token);
    std::optional<int64_t> validate_refresh_token(std::string_view token);

private:
    // Probes the players table for an available username and returns the first
    // free variant of base: base, base_2, base_3, ... up to base_100.
    std::string unique_username(pqxx::transaction_base& txn, std::string_view base);

    // Lowercase + spaces/hyphens → underscore + strip non-[a-z0-9_].
    // Falls back to "player" if the result would be empty.
    static std::string sanitize_username(std::string_view raw);

    DbPool&             pool_;
    PlayerRepo&         repo_;
    const ServerConfig& config_;
    JwtService          jwt_;
};

} // namespace anjeer::server
