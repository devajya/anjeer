#include <catch2/catch_test_macros.hpp>

#include <regex>
#include <thread>

#include <pqxx/pqxx>

#include "server/crypto_util.h"
#include "server/db.h"
#include "server/reconnect_token_repo.h"

using namespace anjeer::server;
using namespace std::chrono_literals;

namespace {

struct Fixture {
    Fixture()
        : pool(TEST_DB_CONN, 2),
          repo(pool)
    {
        DbMigrator migrator(conn, TEST_MIGRATIONS_DIR);
        migrator.run();
        pqxx::work txn(conn);
        txn.exec("TRUNCATE TABLE reconnect_tokens CASCADE");
        txn.exec("TRUNCATE TABLE players RESTART IDENTITY CASCADE");
        txn.exec("TRUNCATE TABLE lobbies CASCADE");

        const auto rp = txn.exec_params(
            "INSERT INTO players (username, oauth_provider, oauth_id) "
            "VALUES ($1,$2,$3) RETURNING id",
            "rtk_test_player", "github", "gh_rtk_1"
        );
        player_id = rp[0][0].as<int64_t>();

        const auto rl = txn.exec_params(
            "INSERT INTO lobbies (code, creator_id, status) "
            "VALUES ($1,$2,'waiting') RETURNING id",
            "RTKTS1", player_id
        );
        lobby_id = rl[0][0].as<std::string>();

        txn.commit();
    }

    pqxx::connection    conn{ TEST_DB_CONN };
    DbPool              pool;
    ReconnectTokenRepo  repo;
    int64_t             player_id;
    std::string         lobby_id;
};

static const std::regex kRtkPattern{ "^rtk_[0-9a-f]{64}$" };

} // namespace

// ---------------------------------------------------------------------------
// generate_reconnect_token (pure crypto)
// ---------------------------------------------------------------------------

TEST_CASE("generate_reconnect_token: starts with rtk_ and is 68 chars", "[crypto]") {
    const auto t = generate_reconnect_token();
    REQUIRE(t.substr(0, 4) == "rtk_");
    REQUIRE(t.size() == 68);
}

TEST_CASE("generate_reconnect_token: matches rtk_[0-9a-f]{64}", "[crypto]") {
    REQUIRE(std::regex_match(generate_reconnect_token(), kRtkPattern));
}

TEST_CASE("generate_reconnect_token: produces unique values", "[crypto]") {
    REQUIRE(generate_reconnect_token() != generate_reconnect_token());
}

// ---------------------------------------------------------------------------
// ReconnectTokenRepo::create
// ---------------------------------------------------------------------------

TEST_CASE("ReconnectTokenRepo: create returns rtk_ prefixed token", "[integration][reconnect_token_repo]") {
    Fixture f;
    const auto tok = f.repo.create(f.player_id, f.lobby_id, 20);
    REQUIRE(std::regex_match(tok, kRtkPattern));
}

TEST_CASE("ReconnectTokenRepo: stored hash differs from plaintext", "[integration][reconnect_token_repo]") {
    Fixture f;
    const auto tok = f.repo.create(f.player_id, f.lobby_id, 20);

    pqxx::work txn(f.conn);
    const auto r = txn.exec_params(
        "SELECT token_hash FROM reconnect_tokens WHERE player_id = $1",
        f.player_id
    );
    txn.commit();

    REQUIRE(r.size() == 1);
    const std::string stored = r[0][0].as<std::string>();
    REQUIRE(stored != tok);
    REQUIRE(stored == sha256_hex(tok));
}

TEST_CASE("ReconnectTokenRepo: create replaces prior token for same player+lobby", "[integration][reconnect_token_repo]") {
    Fixture f;
    const auto tok1 = f.repo.create(f.player_id, f.lobby_id, 20);
    const auto tok2 = f.repo.create(f.player_id, f.lobby_id, 20);
    REQUIRE(tok1 != tok2);

    pqxx::work txn(f.conn);
    const auto r = txn.exec_params(
        "SELECT COUNT(*) FROM reconnect_tokens WHERE player_id = $1",
        f.player_id
    );
    txn.commit();
    REQUIRE(r[0][0].as<int64_t>() == 1);
}

// ---------------------------------------------------------------------------
// ReconnectTokenRepo::validate
// ---------------------------------------------------------------------------

TEST_CASE("ReconnectTokenRepo: validate returns record within window", "[integration][reconnect_token_repo]") {
    Fixture f;
    const auto tok = f.repo.create(f.player_id, f.lobby_id, 20);
    const auto rec = f.repo.validate(tok);

    REQUIRE(rec.has_value());
    REQUIRE(rec->player_id == f.player_id);
    REQUIRE(rec->lobby_id  == f.lobby_id);
    REQUIRE(rec->token_hash == sha256_hex(tok));
    REQUIRE_FALSE(rec->session_id.has_value());
}

TEST_CASE("ReconnectTokenRepo: validate returns nullopt for unknown token", "[integration][reconnect_token_repo]") {
    Fixture f;
    const auto rec = f.repo.validate("rtk_" + std::string(64, '0'));
    REQUIRE_FALSE(rec.has_value());
}

TEST_CASE("ReconnectTokenRepo: validate returns nullopt after expiry", "[integration][reconnect_token_repo]") {
    Fixture f;
    // Insert a row with expires_at already in the past.
    const auto plaintext = generate_reconnect_token();
    const auto hash      = sha256_hex(plaintext);
    {
        pqxx::work txn(f.conn);
        txn.exec_params(
            "INSERT INTO reconnect_tokens (token_hash, player_id, lobby_id, expires_at) "
            "VALUES ($1, $2, $3, NOW() - INTERVAL '1 second')",
            hash, f.player_id, f.lobby_id
        );
        txn.commit();
    }
    REQUIRE_FALSE(f.repo.validate(plaintext).has_value());
}

// ---------------------------------------------------------------------------
// ReconnectTokenRepo::revoke
// ---------------------------------------------------------------------------

TEST_CASE("ReconnectTokenRepo: revoke removes row", "[integration][reconnect_token_repo]") {
    Fixture f;
    const auto tok  = f.repo.create(f.player_id, f.lobby_id, 20);
    const auto hash = sha256_hex(tok);

    f.repo.revoke(hash);

    pqxx::work txn(f.conn);
    const auto r = txn.exec_params(
        "SELECT COUNT(*) FROM reconnect_tokens WHERE token_hash = $1", hash
    );
    txn.commit();
    REQUIRE(r[0][0].as<int64_t>() == 0);
}

TEST_CASE("ReconnectTokenRepo: validate after revoke returns nullopt", "[integration][reconnect_token_repo]") {
    Fixture f;
    const auto tok  = f.repo.create(f.player_id, f.lobby_id, 20);
    const auto hash = sha256_hex(tok);
    f.repo.revoke(hash);
    REQUIRE_FALSE(f.repo.validate(tok).has_value());
}

TEST_CASE("ReconnectTokenRepo: revoke is a no-op for unknown hash", "[integration][reconnect_token_repo]") {
    Fixture f;
    REQUIRE_NOTHROW(f.repo.revoke(std::string(64, '0')));
}

// ---------------------------------------------------------------------------
// ReconnectTokenRepo::cleanup_expired
// ---------------------------------------------------------------------------

TEST_CASE("ReconnectTokenRepo: cleanup_expired deletes only expired rows", "[integration][reconnect_token_repo]") {
    Fixture f;

    // Insert one expired row and one valid row directly — different token_hashes,
    // same player/lobby (application-level uniqueness is enforced by create(), not
    // by a DB unique constraint on (player_id, lobby_id)).
    const auto expired_hash = sha256_hex(generate_reconnect_token());
    const auto fresh_hash   = sha256_hex(generate_reconnect_token());

    {
        pqxx::work txn(f.conn);
        txn.exec_params(
            "INSERT INTO reconnect_tokens (token_hash, player_id, lobby_id, expires_at) "
            "VALUES ($1, $2, $3, NOW() - INTERVAL '1 second')",
            expired_hash, f.player_id, f.lobby_id
        );
        txn.exec_params(
            "INSERT INTO reconnect_tokens (token_hash, player_id, lobby_id, expires_at) "
            "VALUES ($1, $2, $3, NOW() + INTERVAL '60 seconds')",
            fresh_hash, f.player_id, f.lobby_id
        );
        txn.commit();
    }

    f.repo.cleanup_expired();

    pqxx::work txn(f.conn);
    const auto count = txn.exec("SELECT COUNT(*) FROM reconnect_tokens");
    const auto remaining = txn.exec_params(
        "SELECT token_hash FROM reconnect_tokens", pqxx::params{}
    );
    txn.commit();

    REQUIRE(count[0][0].as<int64_t>() == 1);
    REQUIRE(remaining[0][0].as<std::string>() == fresh_hash);
}
