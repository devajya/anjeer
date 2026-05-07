#include "server/api_key_repo.h"

#include "server/crypto_util.h"

#include <sstream>
#include <stdexcept>

#include <pqxx/pqxx>

namespace anjeer::server {

namespace {

// PostgreSQL TIMESTAMPTZ → system_clock::time_point via epoch arithmetic.
std::chrono::system_clock::time_point parse_ts(const std::string& s) {
    // pqxx returns timestamptz as "YYYY-MM-DD HH:MM:SS.ffffff+TZ" or
    // "YYYY-MM-DD HH:MM:SS+TZ". Use pqxx's ztimestamp parser.
    // We store the raw string and convert via pqxx::from_string.
    struct tm tm_val = {};
    std::istringstream ss(s);
    // Parse ISO-8601 date portion only; treat as UTC for internal use.
    ss >> std::get_time(&tm_val, "%Y-%m-%d %H:%M:%S");
    auto tt = std::mktime(&tm_val);  // local time, acceptable for TTL comparisons
    return std::chrono::system_clock::from_time_t(tt);
}

} // namespace

// ── Public methods ─────────────────────────────────────────────────────────────

std::optional<std::string> ApiKeyRepo::create(DbTxn& txn,
                                               int32_t player_id,
                                               const std::string& name,
                                               std::string& err) {
    if (name.empty() || name.size() > 100) {
        err = "VALIDATION_ERROR";
        return std::nullopt;
    }

    // Enforce max-1-active constraint at application layer in addition to
    // the partial unique index (idx_api_keys_one_active_per_player).
    const auto existing = txn.exec_params(
        "SELECT COUNT(*) FROM api_keys "
        "WHERE player_id = $1 AND revoked_at IS NULL AND expires_at > NOW()",
        player_id
    );
    if (existing[0][0].as<int64_t>() > 0) {
        err = "ACTIVE_KEY_EXISTS";
        return std::nullopt;
    }

    const std::string plaintext = generate_api_key();
    const std::string hash      = sha256_hex(plaintext);

    txn.exec_params(
        "INSERT INTO api_keys (player_id, key_hash, name, expires_at) "
        "VALUES ($1, $2, $3, NOW() + INTERVAL '30 days')",
        player_id, hash, name
    );

    return plaintext;
}

std::optional<ApiKeyRecord> ApiKeyRepo::find_valid_by_hash(DbTxn& txn,
                                                            const std::string& key_hash) {
    const auto r = txn.exec_params(
        "SELECT id, player_id, key_hash, name, created_at, expires_at, revoked_at "
        "FROM api_keys "
        "WHERE key_hash = $1 AND revoked_at IS NULL AND expires_at > NOW()",
        key_hash
    );
    if (r.empty()) return std::nullopt;
    return row_to_record(r[0]);
}

std::vector<ApiKeyView> ApiKeyRepo::list_for_player(DbTxn& txn, int32_t player_id) {
    const auto r = txn.exec_params(
        "SELECT id, player_id, name, created_at, expires_at, revoked_at "
        "FROM api_keys "
        "WHERE player_id = $1 "
        "ORDER BY created_at DESC",
        player_id
    );
    std::vector<ApiKeyView> result;
    result.reserve(r.size());
    for (const auto& row : r)
        result.push_back(row_to_view(row));
    return result;
}

bool ApiKeyRepo::revoke(DbTxn& txn, int32_t key_id, int32_t player_id) {
    const auto r = txn.exec_params(
        "UPDATE api_keys SET revoked_at = NOW() "
        "WHERE id = $1 AND player_id = $2 AND revoked_at IS NULL "
        "RETURNING id",
        key_id, player_id
    );
    return !r.empty();
}

// ── Private helpers ────────────────────────────────────────────────────────────

ApiKeyView ApiKeyRepo::row_to_view(const pqxx::row& row) {
    ApiKeyView v;
    v.id        = row[0].as<int32_t>();
    v.player_id = row[1].as<int32_t>();
    v.name      = row[2].as<std::string>();
    v.created_at = parse_ts(row[3].as<std::string>());
    v.expires_at = parse_ts(row[4].as<std::string>());
    if (!row[5].is_null())
        v.revoked_at = parse_ts(row[5].as<std::string>());
    return v;
}

ApiKeyRecord ApiKeyRepo::row_to_record(const pqxx::row& row) {
    ApiKeyRecord rec;
    rec.id        = row[0].as<int32_t>();
    rec.player_id = row[1].as<int32_t>();
    rec.key_hash  = row[2].as<std::string>();
    rec.name      = row[3].as<std::string>();
    rec.created_at = parse_ts(row[4].as<std::string>());
    rec.expires_at = parse_ts(row[5].as<std::string>());
    if (!row[6].is_null())
        rec.revoked_at = parse_ts(row[6].as<std::string>());
    return rec;
}

std::chrono::system_clock::time_point ApiKeyRepo::parse_timestamp(const std::string& s) {
    return parse_ts(s);
}

} // namespace anjeer::server
