#include <catch2/catch_test_macros.hpp>

#include "server/auth_service.h"
#include "server/config.h"
#include "server/db.h"
#include "server/oauth_provider.h"
#include "server/player_repo.h"

// AGENT-CTX: Integration tests require a live PostgreSQL instance at
// anjeer_test (connection string from test_config.json).
// Setup: createdb anjeer_test && make db-migrate DB_CONN=postgresql://localhost:5432/anjeer_test
// These tests are registered under the [integration] tag and run as part of
// make test-unit via the auth_integration_tests binary (see server/CMakeLists.txt).
// Each test truncates the players table in a fixture to keep tests independent.

namespace {

// AGENT-CTX: TestDbFixture provides a migration-run connection and per-test
// table truncation. It does NOT use transactions for cleanup — TRUNCATE is
// DDL-like and ensures a clean slate even if a prior test left uncommitted state.
// The fixture constructs its own pqxx::connection to avoid sharing with DbPool
// (which is not constructed in tests that don't need pool semantics).
struct TestDbFixture {
    explicit TestDbFixture()
        : conn(TEST_DB_CONN)
    {
        anjeer::server::DbMigrator migrator(conn, TEST_MIGRATIONS_DIR);
        migrator.run();
        pqxx::work txn(conn);
        txn.exec("TRUNCATE TABLE players RESTART IDENTITY CASCADE");
        txn.commit();
    }

    pqxx::connection          conn;
    anjeer::server::PlayerRepo repo;
};

} // namespace

// ---------------------------------------------------------------------------
// PlayerRepo tests
// ---------------------------------------------------------------------------

TEST_CASE("find_by_oauth: returns nullopt for unknown player", "[integration][player_repo]") {
    TestDbFixture f;
    pqxx::work    txn(f.conn);
    auto result = f.repo.find_by_oauth(txn, "github", "no_such_id");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("insert: new player row has expected fields", "[integration][player_repo]") {
    TestDbFixture f;
    pqxx::work    txn(f.conn);
    const auto player = f.repo.insert(txn, "alice", "github", "gh_alice");
    txn.commit();

    REQUIRE(player.username       == "alice");
    REQUIRE(player.oauth_provider == "github");
    REQUIRE(player.oauth_id       == "gh_alice");
    REQUIRE(player.games_played   == 0);
    REQUIRE(player.id             > 0);
}

TEST_CASE("insert + find_by_oauth: round-trip", "[integration][player_repo]") {
    TestDbFixture f;
    {
        pqxx::work txn(f.conn);
        f.repo.insert(txn, "bob", "google", "go_bob");
        txn.commit();
    }
    pqxx::work txn(f.conn);
    const auto found = f.repo.find_by_oauth(txn, "google", "go_bob");
    REQUIRE(found.has_value());
    REQUIRE(found->username == "bob");
}

TEST_CASE("find_by_id: returns correct player", "[integration][player_repo]") {
    TestDbFixture f;
    int64_t inserted_id{};
    {
        pqxx::work txn(f.conn);
        const auto p = f.repo.insert(txn, "carol", "github", "gh_carol");
        inserted_id = p.id;
        txn.commit();
    }
    pqxx::work txn(f.conn);
    const auto found = f.repo.find_by_id(txn, inserted_id);
    REQUIRE(found.has_value());
    REQUIRE(found->username == "carol");
}

TEST_CASE("find_by_id: returns nullopt for missing id", "[integration][player_repo]") {
    TestDbFixture f;
    pqxx::work    txn(f.conn);
    REQUIRE_FALSE(f.repo.find_by_id(txn, 999999).has_value());
}

TEST_CASE("insert: same (provider, oauth_id) twice throws unique_violation", "[integration][player_repo]") {
    TestDbFixture f;
    {
        pqxx::work txn(f.conn);
        f.repo.insert(txn, "dan", "github", "gh_dan");
        txn.commit();
    }
    // AGENT-CTX: pqxx::unique_violation is what AuthService catches to detect
    // duplicate oauth accounts. The exception type here must match the catch
    // clause in AuthService::find_or_create — if pqxx changes the exception
    // hierarchy in a version upgrade, update both this test and auth_service.cpp.
    REQUIRE_THROWS_AS(
        [&] {
            pqxx::work txn(f.conn);
            f.repo.insert(txn, "dan2", "github", "gh_dan");
            txn.commit();
        }(),
        pqxx::unique_violation
    );
}

TEST_CASE("insert: same username twice throws unique_violation", "[integration][player_repo]") {
    TestDbFixture f;
    {
        pqxx::work txn(f.conn);
        f.repo.insert(txn, "eve", "github", "gh_eve");
        txn.commit();
    }
    REQUIRE_THROWS_AS(
        [&] {
            pqxx::work txn(f.conn);
            f.repo.insert(txn, "eve", "google", "go_eve");
            txn.commit();
        }(),
        pqxx::unique_violation
    );
}

TEST_CASE("DbMigrator: idempotent — running twice does not throw", "[integration][db]") {
    TestDbFixture f;
    // run() was already called in the fixture constructor; calling again must be safe.
    REQUIRE_NOTHROW([&] {
        anjeer::server::DbMigrator migrator(f.conn, TEST_MIGRATIONS_DIR);
        migrator.run();
    }());
}

// =============================================================================
// OAuth provider unit tests — no real HTTP, no DB
// =============================================================================

namespace {

// AGENT-CTX: OAuthProviderConfig for tests. client_id/secret are arbitrary
// strings; redirect_uri must survive url_encode round-trip but need not be real.
anjeer::server::ServerConfig::AuthConfig::OAuthProviderConfig test_provider_cfg() {
    return {"test_client_id", "test_client_secret",
            "http://localhost:8080/auth/test/callback"};
}

} // namespace

// ---------------------------------------------------------------------------
// GitHubOAuthProvider
// ---------------------------------------------------------------------------

TEST_CASE("GitHubOAuthProvider: authorization_url has correct base and required params",
          "[oauth][github]") {
    using namespace anjeer::server;
    GitHubOAuthProvider p(test_provider_cfg());
    const auto url = p.authorization_url("nonce123");
    REQUIRE(url.rfind("https://github.com/login/oauth/authorize", 0) == 0);
    REQUIRE(url.find("client_id=test_client_id") != std::string::npos);
    REQUIRE(url.find("state=nonce123")           != std::string::npos);
}

TEST_CASE("GitHubOAuthProvider: exchange_code parses OAuthUserInfo correctly",
          "[oauth][github]") {
    using namespace anjeer::server;

    const std::string token_resp   = R"({"access_token":"gho_tok","token_type":"bearer"})";
    const std::string profile_resp = R"({"id":12345,"login":"devuser","email":"dev@example.com"})";

    GitHubOAuthProvider p(
        test_provider_cfg(),
        [&](std::string_view, std::string_view) { return profile_resp; },
        [&](std::string_view, std::string_view, std::string_view) { return token_resp; }
    );

    const auto info = p.exchange_code("auth_code");
    REQUIRE(info.provider_id  == "12345");
    REQUIRE(info.username     == "devuser");
    REQUIRE(info.email.has_value());
    REQUIRE(info.email.value() == "dev@example.com");
}

TEST_CASE("GitHubOAuthProvider: null email in profile gives nullopt email",
          "[oauth][github]") {
    using namespace anjeer::server;

    const std::string token_resp   = R"({"access_token":"tok"})";
    const std::string profile_resp = R"({"id":99,"login":"anon","email":null})";

    GitHubOAuthProvider p(
        test_provider_cfg(),
        [&](std::string_view, std::string_view) { return profile_resp; },
        [&](std::string_view, std::string_view, std::string_view) { return token_resp; }
    );

    const auto info = p.exchange_code("code");
    REQUIRE(info.provider_id == "99");
    REQUIRE_FALSE(info.email.has_value());
}

TEST_CASE("GitHubOAuthProvider: error response from provider throws runtime_error",
          "[oauth][github]") {
    using namespace anjeer::server;

    const std::string error_resp =
        R"({"error":"bad_verification_code","error_description":"The code passed is incorrect."})";

    GitHubOAuthProvider p(
        test_provider_cfg(),
        [&](std::string_view, std::string_view) { return ""; },
        [&](std::string_view, std::string_view, std::string_view) { return error_resp; }
    );

    REQUIRE_THROWS_AS(p.exchange_code("bad_code"), std::runtime_error);
}

// ---------------------------------------------------------------------------
// GoogleOAuthProvider
// ---------------------------------------------------------------------------

TEST_CASE("GoogleOAuthProvider: authorization_url has correct base and required params",
          "[oauth][google]") {
    using namespace anjeer::server;
    GoogleOAuthProvider p(test_provider_cfg());
    const auto url = p.authorization_url("google_nonce");
    REQUIRE(url.rfind("https://accounts.google.com/o/oauth2/v2/auth", 0) == 0);
    REQUIRE(url.find("client_id=test_client_id") != std::string::npos);
    REQUIRE(url.find("state=google_nonce")        != std::string::npos);
    REQUIRE(url.find("response_type=code")        != std::string::npos);
}

TEST_CASE("GoogleOAuthProvider: exchange_code parses OAuthUserInfo correctly",
          "[oauth][google]") {
    using namespace anjeer::server;

    const std::string token_resp   = R"({"access_token":"ya29_tok","token_type":"Bearer"})";
    const std::string profile_resp =
        R"({"sub":"11111","name":"Jane Doe","given_name":"Jane","email":"jane@gmail.com"})";

    GoogleOAuthProvider p(
        test_provider_cfg(),
        [&](std::string_view, std::string_view) { return profile_resp; },
        [&](std::string_view, std::string_view, std::string_view) { return token_resp; }
    );

    const auto info = p.exchange_code("g_code");
    REQUIRE(info.provider_id  == "11111");
    REQUIRE(info.username     == "Jane");   // given_name preferred
    REQUIRE(info.email.has_value());
    REQUIRE(info.email.value() == "jane@gmail.com");
}

TEST_CASE("GoogleOAuthProvider: falls back to name when given_name absent",
          "[oauth][google]") {
    using namespace anjeer::server;

    const std::string token_resp   = R"({"access_token":"tok"})";
    const std::string profile_resp = R"({"sub":"22222","name":"Service Account"})";

    GoogleOAuthProvider p(
        test_provider_cfg(),
        [&](std::string_view, std::string_view) { return profile_resp; },
        [&](std::string_view, std::string_view, std::string_view) { return token_resp; }
    );

    const auto info = p.exchange_code("code");
    REQUIRE(info.username == "Service Account");
}

// =============================================================================
// JwtService unit tests — no DB, no network
// =============================================================================

// AGENT-CTX: JWT secret must be at least 32 bytes for HMAC-SHA256.
// Shorter strings compile and sign fine but are cryptographically weak.
// The exact boundary is enforced by the standard, not by jwt-cpp.
static constexpr const char* kTestSecret = "test-jwt-secret-32-bytes-exactly!";

TEST_CASE("JwtService: issue and validate_access returns correct player id",
          "[jwt]") {
    anjeer::server::JwtService jwt(kTestSecret, 900, 86400);
    anjeer::server::Player p{42, "alice", "github", "gh_1", 1000};
    const auto tokens    = jwt.issue(p);
    const auto player_id = jwt.validate_access(tokens.access_token);
    REQUIRE(player_id.has_value());
    REQUIRE(player_id.value() == 42);
}

TEST_CASE("JwtService: issue and validate_refresh returns correct player id",
          "[jwt]") {
    anjeer::server::JwtService jwt(kTestSecret, 900, 86400);
    anjeer::server::Player p{7, "bob", "google", "go_7", 500};
    const auto tokens    = jwt.issue(p);
    const auto player_id = jwt.validate_refresh(tokens.refresh_token);
    REQUIRE(player_id.has_value());
    REQUIRE(player_id.value() == 7);
}

TEST_CASE("JwtService: access token rejected by validate_refresh", "[jwt]") {
    anjeer::server::JwtService jwt(kTestSecret, 900, 86400);
    anjeer::server::Player p{1, "x", "github", "gh_x", 0};
    const auto tokens = jwt.issue(p);
    REQUIRE_FALSE(jwt.validate_refresh(tokens.access_token).has_value());
}

TEST_CASE("JwtService: refresh token rejected by validate_access", "[jwt]") {
    anjeer::server::JwtService jwt(kTestSecret, 900, 86400);
    anjeer::server::Player p{1, "x", "github", "gh_x", 0};
    const auto tokens = jwt.issue(p);
    REQUIRE_FALSE(jwt.validate_access(tokens.refresh_token).has_value());
}

TEST_CASE("JwtService: expired token returns nullopt", "[jwt]") {
    // AGENT-CTX: Negative TTL causes set_expires_at(now - 1s) = already expired.
    // This is the standard way to test expiry without sleeping.
    anjeer::server::JwtService jwt(kTestSecret, -1, 86400);
    anjeer::server::Player p{1, "x", "github", "gh_x", 0};
    const auto tokens = jwt.issue(p);
    REQUIRE_FALSE(jwt.validate_access(tokens.access_token).has_value());
}

TEST_CASE("JwtService: token signed with different secret returns nullopt", "[jwt]") {
    anjeer::server::JwtService jwt_a("secret-a-exactly-32-bytes-longXX", 900, 86400);
    anjeer::server::JwtService jwt_b("secret-b-exactly-32-bytes-longXX", 900, 86400);
    anjeer::server::Player p{1, "x", "github", "gh_x", 0};
    const auto tokens = jwt_a.issue(p);
    REQUIRE_FALSE(jwt_b.validate_access(tokens.access_token).has_value());
}

// =============================================================================
// AuthService integration tests — require live anjeer_test DB
// =============================================================================

namespace {

// AGENT-CTX: AuthTestFixture owns a DbPool (not just a bare connection) because
// AuthService::find_or_create calls pool_.acquire() internally. Pool size 2 is
// sufficient: one connection for the fixture setup, one for the service call.
// ServerConfig is value-initialised then populated minimally — only the fields
// AuthService actually reads are set. This keeps the fixture resilient to
// future config additions without requiring fixture changes.
struct AuthTestFixture {
    static anjeer::server::ServerConfig make_config() {
        anjeer::server::ServerConfig cfg{};
        cfg.auth.jwt_secret                = kTestSecret;
        cfg.auth.access_token_ttl_seconds  = 900;
        cfg.auth.refresh_token_ttl_seconds = 86400;
        cfg.auth.secure_cookies            = false;
        return cfg;
    }

    explicit AuthTestFixture()
        : config(make_config())
        , pool(TEST_DB_CONN, 2)
        , service(pool, repo, config)
    {
        pqxx::connection conn(TEST_DB_CONN);
        anjeer::server::DbMigrator migrator(conn, TEST_MIGRATIONS_DIR);
        migrator.run();
        pqxx::work txn(conn);
        txn.exec("TRUNCATE TABLE players RESTART IDENTITY CASCADE");
        txn.commit();
    }

    anjeer::server::ServerConfig config;
    anjeer::server::DbPool       pool;
    anjeer::server::PlayerRepo   repo;
    anjeer::server::AuthService  service;
};

} // namespace

TEST_CASE("AuthService: find_or_create creates new player",
          "[integration][auth_service]") {
    AuthTestFixture f;
    anjeer::server::OAuthUserInfo info{"gh_new_123", "newuser", std::nullopt};
    const auto player = f.service.find_or_create("github", info);
    REQUIRE(player.username       == "newuser");
    REQUIRE(player.oauth_provider == "github");
    REQUIRE(player.oauth_id       == "gh_new_123");
    REQUIRE(player.games_played   == 0);
    REQUIRE(player.id             > 0);
}

TEST_CASE("AuthService: find_or_create is idempotent for same provider+id",
          "[integration][auth_service]") {
    AuthTestFixture f;
    anjeer::server::OAuthUserInfo info{"gh_idem", "idem_user", std::nullopt};
    const auto first  = f.service.find_or_create("github", info);
    const auto second = f.service.find_or_create("github", info);
    REQUIRE(first.id == second.id);
}

TEST_CASE("AuthService: find_or_create resolves username collision with suffix",
          "[integration][auth_service]") {
    AuthTestFixture f;
    // Two different OAuth identities that would produce the same sanitized username
    anjeer::server::OAuthUserInfo info1{"gh_alpha", "collision_user", std::nullopt};
    anjeer::server::OAuthUserInfo info2{"go_alpha", "collision_user", std::nullopt};
    const auto p1 = f.service.find_or_create("github",  info1);
    const auto p2 = f.service.find_or_create("google", info2);
    REQUIRE(p1.username != p2.username);
    REQUIRE(p2.username == "collision_user_2");
}

TEST_CASE("AuthService: sanitizes username with spaces",
          "[integration][auth_service]") {
    AuthTestFixture f;
    anjeer::server::OAuthUserInfo info{"gh_spaces", "John Doe", "john@example.com"};
    const auto player = f.service.find_or_create("github", info);
    REQUIRE(player.username == "john_doe");
}

TEST_CASE("AuthService: issue_tokens produces valid access and refresh tokens",
          "[integration][auth_service]") {
    AuthTestFixture f;
    anjeer::server::OAuthUserInfo info{"gh_jwt_test", "jwtuser", std::nullopt};
    const auto player = f.service.find_or_create("github", info);
    const auto tokens = f.service.issue_tokens(player);

    const auto access_id  = f.service.validate_access_token(tokens.access_token);
    const auto refresh_id = f.service.validate_refresh_token(tokens.refresh_token);

    REQUIRE(access_id.has_value());
    REQUIRE(access_id.value()  == player.id);
    REQUIRE(refresh_id.has_value());
    REQUIRE(refresh_id.value() == player.id);
}

TEST_CASE("AuthService: validate_access_token rejects expired token",
          "[integration][auth_service]") {
    // AGENT-CTX: We build a JwtService with access_ttl=-1 directly to get an
    // already-expired token, then verify that AuthService (which uses its own
    // JwtService from config) correctly rejects it. The two JwtService instances
    // share the same secret, so the signature is valid — only the expiry fails.
    AuthTestFixture f;
    anjeer::server::Player dummy{1, "x", "github", "gh_x", 0};
    anjeer::server::JwtService expired_jwt(kTestSecret, -1, 86400);
    const auto tokens = expired_jwt.issue(dummy);
    REQUIRE_FALSE(f.service.validate_access_token(tokens.access_token).has_value());
}
