#include "server/auth_service.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include <jwt-cpp/jwt.h>
#include <pqxx/pqxx>

namespace anjeer::server {

// ---------------------------------------------------------------------------
// JwtService
// ---------------------------------------------------------------------------

JwtService::JwtService(std::string_view secret, int access_ttl, int refresh_ttl)
    : secret_(secret), access_ttl_(access_ttl), refresh_ttl_(refresh_ttl) {}

JwtService::TokenPair JwtService::issue(const Player& player) const {
    const auto now     = std::chrono::system_clock::now();
    const auto subject = std::to_string(player.id);

    // AGENT-CTX: "type" is a custom claim — it is NOT the standard JWT "typ"
    // header (which jwt-cpp calls set_type and always sets to "JWT"). The custom
    // claim distinguishes access from refresh at validation time. Never use the
    // standard "typ" header for this purpose; it is not part of the verified
    // payload and providers/proxies may strip or rewrite it.
    const auto access = jwt::create()
        .set_type("JWT")
        .set_issuer("anjeer")
        .set_subject(subject)
        .set_issued_at(now)
        .set_expires_at(now + std::chrono::seconds(access_ttl_))
        .set_payload_claim("type", jwt::claim(std::string("access")))
        .sign(jwt::algorithm::hs256{secret_});

    const auto refresh = jwt::create()
        .set_type("JWT")
        .set_issuer("anjeer")
        .set_subject(subject)
        .set_issued_at(now)
        .set_expires_at(now + std::chrono::seconds(refresh_ttl_))
        .set_payload_claim("type", jwt::claim(std::string("refresh")))
        .sign(jwt::algorithm::hs256{secret_});

    return {access, refresh};
}

std::optional<int64_t> JwtService::validate_token(std::string_view token,
                                                    std::string_view expected_type) const {
    // AGENT-CTX: Catching std::exception is intentional — jwt-cpp throws
    // different exception types for different failure modes (expired, bad
    // signature, missing claim, malformed base64). All failure modes should
    // return nullopt to the caller; the distinction is irrelevant for auth.
    // Do not narrow the catch to a specific jwt exception type without also
    // handling std::invalid_argument (thrown by std::stoll on a non-numeric
    // subject).
    try {
        const auto decoded = jwt::decode(std::string(token));
        jwt::verify()
            .allow_algorithm(jwt::algorithm::hs256{secret_})
            .with_issuer("anjeer")
            .with_claim("type", jwt::claim(std::string(expected_type)))
            .verify(decoded);
        return std::stoll(decoded.get_subject());
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<int64_t> JwtService::validate_access(std::string_view token) const {
    return validate_token(token, "access");
}

std::optional<int64_t> JwtService::validate_refresh(std::string_view token) const {
    return validate_token(token, "refresh");
}

// ---------------------------------------------------------------------------
// AuthService
// ---------------------------------------------------------------------------

AuthService::AuthService(DbPool& pool, PlayerRepo& repo, const ServerConfig& config)
    : pool_(pool)
    , repo_(repo)
    , config_(config)
    , jwt_(config.auth.jwt_secret,
           config.auth.access_token_ttl_seconds,
           config.auth.refresh_token_ttl_seconds)
{}

std::string AuthService::sanitize_username(std::string_view raw) {
    // Falls back to "player" so insert never receives an empty string.
    std::string out;
    out.reserve(raw.size());
    for (const unsigned char c : raw) {
        if (std::isalnum(c)) {
            out += static_cast<char>(std::tolower(static_cast<int>(c)));
        } else if (c == ' ' || c == '-' || c == '_') {
            out += '_';
        }
        // all other characters dropped
    }

    const auto first = out.find_first_not_of('_');
    if (first == std::string::npos) return "player";
    const auto last = out.find_last_not_of('_');
    out = out.substr(first, last - first + 1);
    return out.empty() ? "player" : out;
}

std::string AuthService::unique_username(pqxx::transaction_base& txn,
                                          std::string_view        base) {
    // Single query fetches base and all _N variants. LIKE pattern is
    // intentionally broad (any suffix after _) — false positives are harmless
    // since we do exact-match lookup on the client side. TOCTOU race is
    // acceptable: the oauth_provider+oauth_id UNIQUE constraint prevents a
    // second player row for the same identity, and username races are handled
    // by the pqxx::unique_violation retry in find_or_create.
    // Future: replace with an adjective-noun-NNN generator (see project TODOs).
    const std::string base_str(base);
    const auto rows = txn.exec_params(
        "SELECT username FROM players WHERE username = $1 OR username LIKE $1 || '\\_%'",
        base_str
    );

    std::unordered_set<std::string> taken;
    taken.reserve(rows.size());
    for (const auto& row : rows) taken.insert(row[0].as<std::string>());

    if (!taken.count(base_str)) return base_str;

    for (int i = 2; i <= 100; ++i) {
        const std::string candidate = base_str + "_" + std::to_string(i);
        if (!taken.count(candidate)) return candidate;
    }
    throw std::runtime_error("unique_username: no free slot found after 100 attempts");
}

Player AuthService::find_or_create(std::string_view provider,
                                    const OAuthUserInfo& info) {
    // AGENT-CTX: Two-attempt retry handles the concurrent-login race:
    // two requests for the same OAuth identity both miss find_by_oauth, but
    // only one wins the INSERT (UNIQUE on oauth_provider+oauth_id). The loser
    // catches pqxx::unique_violation, its transaction is aborted, and it
    // retries — the second find_by_oauth will succeed.
    // Two attempts are sufficient; a third failure indicates a bug elsewhere.
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto handle = pool_.acquire();
        pqxx::work txn(handle.get());  // BEGIN

        if (auto existing = repo_.find_by_oauth(txn, provider, info.provider_id)) {
            txn.commit();
            return *existing;
        }

        try {
            const std::string sanitized = sanitize_username(info.username);
            const std::string username  = unique_username(txn, sanitized);
            const auto player = repo_.insert(txn, username, provider, info.provider_id);
            txn.commit();
            return player;
        } catch (const pqxx::unique_violation&) {
            // pqxx::work destructor calls ROLLBACK automatically here
            // because we did not commit. Next iteration retries.
        }
    }

    throw std::runtime_error("find_or_create: still failing after 2 attempts — "
                              "possible data integrity issue");
}

JwtService::TokenPair AuthService::issue_tokens(const Player& player) {
    return jwt_.issue(player);
}

std::optional<int64_t> AuthService::validate_access_token(std::string_view token) {
    return jwt_.validate_access(token);
}

std::optional<int64_t> AuthService::validate_refresh_token(std::string_view token) {
    return jwt_.validate_refresh(token);
}

} // namespace anjeer::server
