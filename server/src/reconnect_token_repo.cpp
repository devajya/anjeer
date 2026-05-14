#include "server/reconnect_token_repo.h"

#include "server/crypto_util.h"

#include <sstream>
#include <stdexcept>

#include <pqxx/pqxx>

namespace anjeer::server {

ReconnectTokenRepo::ReconnectTokenRepo(DbPool& pool) : pool_(pool) {}

// ── Public methods ─────────────────────────────────────────────────────────────

std::string ReconnectTokenRepo::create(int64_t player_id,
                                       const std::string& lobby_id,
                                       int window_seconds) {
    const std::string plaintext = generate_reconnect_token();
    const std::string hash      = sha256_hex(plaintext);

    auto handle = pool_.acquire();
    pqxx::work txn(handle.get());

    // Replace any prior token for this (player_id, lobby_id) pair so there is
    // always at most one valid reconnect token per slot.
    txn.exec_params(
        "DELETE FROM reconnect_tokens WHERE player_id = $1 AND lobby_id = $2",
        player_id, lobby_id
    );

    txn.exec_params(
        "INSERT INTO reconnect_tokens (token_hash, player_id, lobby_id, expires_at) "
        "VALUES ($1, $2, $3, NOW() + ($4 || ' seconds')::INTERVAL)",
        hash, player_id, lobby_id, std::to_string(window_seconds)
    );

    txn.commit();
    return plaintext;
}

std::optional<ReconnectTokenRecord> ReconnectTokenRepo::validate(
    const std::string& token_plaintext)
{
    const std::string hash = sha256_hex(token_plaintext);

    auto handle = pool_.acquire();
    pqxx::work txn(handle.get());

    const auto r = txn.exec_params(
        "SELECT token_hash, player_id, lobby_id, session_id, expires_at "
        "FROM reconnect_tokens "
        "WHERE token_hash = $1 AND expires_at > NOW()",
        hash
    );
    txn.commit();

    if (r.empty()) return std::nullopt;

    ReconnectTokenRecord rec;
    rec.token_hash = r[0][0].as<std::string>();
    rec.player_id  = r[0][1].as<int64_t>();
    rec.lobby_id   = r[0][2].as<std::string>();
    if (!r[0][3].is_null())
        rec.session_id = r[0][3].as<std::string>();
    rec.expires_at = parse_ts(r[0][4].as<std::string>());

    return rec;
}

void ReconnectTokenRepo::revoke(const std::string& token_hash) {
    auto handle = pool_.acquire();
    pqxx::work txn(handle.get());
    txn.exec_params(
        "DELETE FROM reconnect_tokens WHERE token_hash = $1",
        token_hash
    );
    txn.commit();
}

void ReconnectTokenRepo::cleanup_expired() {
    auto handle = pool_.acquire();
    pqxx::work txn(handle.get());
    txn.exec("DELETE FROM reconnect_tokens WHERE expires_at <= NOW()");
    txn.commit();
}

// ── Private helpers ────────────────────────────────────────────────────────────

std::chrono::system_clock::time_point ReconnectTokenRepo::parse_ts(const std::string& s) {
    struct tm tm_val = {};
    std::istringstream ss(s);
    ss >> std::get_time(&tm_val, "%Y-%m-%d %H:%M:%S");
    auto tt = std::mktime(&tm_val);
    return std::chrono::system_clock::from_time_t(tt);
}

} // namespace anjeer::server
