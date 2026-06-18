#include <catch2/catch_test_macros.hpp>
#include "server/spectate_token_repo.h"
#include "server/db.h"
#include <pqxx/pqxx>
#include <cstdlib>
#include <thread>
#include <chrono>

using namespace anjeer::server;
using namespace std::chrono_literals;

namespace {
struct SpectateFixture {
    SpectateFixture() : conn(TEST_DB_CONN), repo(1) {
        DbMigrator migrator(conn, TEST_MIGRATIONS_DIR);
        migrator.run();
        pqxx::work txn(conn);
        txn.exec("TRUNCATE TABLE spectate_tokens CASCADE");
        txn.exec("TRUNCATE TABLE lobbies CASCADE");
        txn.exec("TRUNCATE TABLE players RESTART IDENTITY CASCADE");
        const auto rp = txn.exec_params(
            "INSERT INTO players (username, oauth_provider, oauth_id) "
            "VALUES ($1,$2,$3) RETURNING id",
            "spectate_test_player", "github", "gh_spect_1"
        );
        player_id = rp[0][0].as<int64_t>();
        const auto rl = txn.exec_params(
            "INSERT INTO lobbies (code, creator_id, status) "
            "VALUES ($1,$2,'waiting') RETURNING id",
            "SPCT01", player_id
        );
        lobby_id = rl[0][0].as<std::string>();
        txn.commit();
    }

    pqxx::connection  conn;
    SpectateTokenRepo repo;
    int64_t           player_id;
    std::string       lobby_id;
};
} // namespace

TEST_CASE("SpectateTokenRepo::cleanup_expired removes expired tokens", "[spectate][db]") {
    SpectateFixture f;

    // Create a token with 1-minute TTL and then backdate its expires_at to the past.
    {
        pqxx::work txn(f.conn);
        f.repo.create(txn, f.player_id, "SPCT01");
        txn.exec("UPDATE spectate_tokens SET expires_at = NOW() - INTERVAL '1 second'");
        txn.commit();
    }

    {
        pqxx::work txn(f.conn);
        const auto before = txn.exec("SELECT COUNT(*) FROM spectate_tokens");
        REQUIRE(before[0][0].as<int>() == 1);
        f.repo.cleanup_expired(txn);
        txn.commit();
    }

    pqxx::work txn(f.conn);
    const auto after = txn.exec("SELECT COUNT(*) FROM spectate_tokens");
    REQUIRE(after[0][0].as<int>() == 0);
}

TEST_CASE("SpectateTokenRepo::cleanup_expired leaves valid tokens intact", "[spectate][db]") {
    SpectateFixture f;

    {
        pqxx::work txn(f.conn);
        f.repo.create(txn, f.player_id, "SPCT01");
        txn.commit();
    }

    {
        pqxx::work txn(f.conn);
        f.repo.cleanup_expired(txn);
        txn.commit();
    }

    pqxx::work txn(f.conn);
    const auto after = txn.exec("SELECT COUNT(*) FROM spectate_tokens");
    REQUIRE(after[0][0].as<int>() == 1);
}
