#include "server/spectate_token_repo.h"

#include "server/crypto_util.h"

#include <openssl/rand.h>
#include <sstream>
#include <iomanip>
#include <stdexcept>

#include <pqxx/pqxx>

namespace anjeer::server {

namespace {

std::string generate_spectate_token() {
    unsigned char buf[32];
    if (RAND_bytes(buf, sizeof(buf)) != 1)
        throw std::runtime_error("RAND_bytes failed in generate_spectate_token");
    std::ostringstream oss;
    oss << "stk_";
    for (unsigned char b : buf)
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    return oss.str();
}

} // namespace

SpectateTokenRepo::SpectateTokenRepo(int ttl_minutes)
    : ttl_minutes_(ttl_minutes)
{}

SpectateToken SpectateTokenRepo::create(DbTxn& txn,
                                         int64_t player_id,
                                         const std::string& lobby_code) {
    const std::string plaintext = generate_spectate_token();
    const std::string hash      = sha256_hex(plaintext);

    const auto r = txn.exec_params(
        "INSERT INTO spectate_tokens (token_hash, player_id, lobby_code, expires_at) "
        "VALUES ($1, $2, $3, NOW() + ($4 || ' minutes')::INTERVAL) "
        "RETURNING id, token_hash, player_id, lobby_code, expires_at, used_at",
        hash, player_id, lobby_code, std::to_string(ttl_minutes_)
    );

    SpectateToken tok = row_to_token(r[0]);
    tok.plaintext = plaintext;
    return tok;
}

std::optional<SpectateToken> SpectateTokenRepo::find_valid_and_consume(
        DbTxn& txn, const std::string& raw_token) {
    const std::string hash = sha256_hex(raw_token);

    const auto r = txn.exec_params(
        "UPDATE spectate_tokens "
        "SET used_at = NOW() "
        "WHERE token_hash = $1 "
        "  AND expires_at > NOW() "
        "  AND used_at IS NULL "
        "RETURNING id, token_hash, player_id, lobby_code, expires_at, used_at",
        hash
    );
    if (r.empty()) return std::nullopt;
    return row_to_token(r[0]);
}

// ── Private helpers ────────────────────────────────────────────────────────────

SpectateToken SpectateTokenRepo::row_to_token(const pqxx::row& row) {
    SpectateToken tok;
    tok.id         = row[0].as<int64_t>();
    tok.token_hash = row[1].as<std::string>();
    tok.player_id  = row[2].as<int64_t>();
    tok.lobby_code = row[3].as<std::string>();
    tok.expires_at = parse_ts(row[4].as<std::string>());
    if (!row[5].is_null())
        tok.used_at = parse_ts(row[5].as<std::string>());
    return tok;
}

std::chrono::system_clock::time_point SpectateTokenRepo::parse_ts(const std::string& s) {
    struct tm tm_val = {};
    std::istringstream ss(s);
    ss >> std::get_time(&tm_val, "%Y-%m-%d %H:%M:%S");
    auto tt = std::mktime(&tm_val);
    return std::chrono::system_clock::from_time_t(tt);
}

} // namespace anjeer::server
