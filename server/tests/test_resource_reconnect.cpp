#include <catch2/catch_test_macros.hpp>

#include "server/db.h"
#include "server/reconnect_token_repo.h"

#include <pqxx/pqxx>
#include <chrono>
#include <thread>

using namespace anjeer::server;
using namespace std::chrono_literals;

// ── Class 7: reconnect token expiry tests ─────────────────────────────────────

namespace {
struct ReconnectFixture {
    ReconnectFixture()
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
            "reconnect_res_player", "github", "gh_reconres_1"
        );
        player_id = rp[0][0].as<int64_t>();

        const auto rl = txn.exec_params(
            "INSERT INTO lobbies (code, creator_id, status) "
            "VALUES ($1,$2,'waiting') RETURNING id",
            "RCN001", player_id
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
} // namespace

// validate() returns nullopt for a token created with a 1-second window after it expires.
TEST_CASE("ReconnectTokenRepo validate returns nullopt after token expires", "[resource][reconnect][db]") {
    ReconnectFixture f;

    const std::string token = f.repo.create(f.player_id, f.lobby_id, /*window_seconds=*/1);
    REQUIRE(f.repo.validate(token).has_value());

    std::this_thread::sleep_for(1100ms);

    REQUIRE_FALSE(f.repo.validate(token).has_value());
}

// cleanup_expired deletes the expired row.
TEST_CASE("ReconnectTokenRepo cleanup_expired removes expired rows", "[resource][reconnect][db]") {
    ReconnectFixture f;

    f.repo.create(f.player_id, f.lobby_id, /*window_seconds=*/1);
    {
        pqxx::work txn(f.conn);
        const auto before = txn.exec("SELECT COUNT(*) FROM reconnect_tokens");
        REQUIRE(before[0][0].as<int>() == 1);
    }

    std::this_thread::sleep_for(1100ms);

    f.repo.cleanup_expired();

    pqxx::work txn(f.conn);
    const auto after = txn.exec("SELECT COUNT(*) FROM reconnect_tokens");
    REQUIRE(after[0][0].as<int>() == 0);
}

// cleanup_expired leaves a still-valid token alone.
TEST_CASE("ReconnectTokenRepo cleanup_expired leaves valid token intact", "[resource][reconnect][db]") {
    ReconnectFixture f;

    f.repo.create(f.player_id, f.lobby_id, /*window_seconds=*/3600);
    f.repo.cleanup_expired();

    pqxx::work txn(f.conn);
    const auto count = txn.exec("SELECT COUNT(*) FROM reconnect_tokens");
    REQUIRE(count[0][0].as<int>() == 1);
}
