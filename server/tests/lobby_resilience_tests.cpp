#include <catch2/catch_test_macros.hpp>

#include "server/db.h"
#include "server/lobby_repo.h"

namespace {

struct Fixture {
    explicit Fixture() : conn(TEST_DB_CONN) {
        anjeer::server::DbMigrator migrator(conn, TEST_MIGRATIONS_DIR);
        migrator.run();
        pqxx::work txn(conn);
        txn.exec("TRUNCATE TABLE lobbies CASCADE");
        txn.exec("TRUNCATE TABLE players RESTART IDENTITY CASCADE");

        const auto r = txn.exec_params(
            "INSERT INTO players (username, oauth_provider, oauth_id) "
            "VALUES ($1, $2, $3) RETURNING id",
            "creator", "github", "gh_creator_1"
        );
        creator_id = r[0][0].as<int64_t>();
        txn.commit();
    }

    int64_t insert_player(const std::string& username, const std::string& oauth_id) {
        pqxx::work txn(conn);
        const auto r = txn.exec_params(
            "INSERT INTO players (username, oauth_provider, oauth_id) "
            "VALUES ($1, $2, $3) RETURNING id",
            username, "github", oauth_id
        );
        auto id = r[0][0].as<int64_t>();
        txn.commit();
        return id;
    }

    pqxx::connection          conn;
    anjeer::server::LobbyRepo repo;
    int64_t                   creator_id;
};

} // namespace

// L1 + L4: create() inserts creator row → player_count == 1 immediately
TEST_CASE("L1/L4 lobby creation inserts creator row giving count of 1", "[integration][lobby_resilience]") {
    Fixture f;
    pqxx::work txn(f.conn);
    const auto lobby = f.repo.create(txn, f.creator_id, 2, 8);
    txn.commit();

    pqxx::work check(f.conn);
    const auto count = f.repo.player_count(check, lobby.id);
    REQUIRE(count == 1);
}

// L2: leaving decrements count
TEST_CASE("L2 leave_lobby decrements player count", "[integration][lobby_resilience]") {
    Fixture f;
    const auto p2 = f.insert_player("player2", "gh_p2");

    std::string lobby_id;
    {
        pqxx::work txn(f.conn);
        lobby_id = f.repo.create(txn, f.creator_id, 2, 8).id;
        f.repo.add_player(txn, lobby_id, p2);
        txn.commit();
    }

    {
        pqxx::work check(f.conn);
        REQUIRE(f.repo.player_count(check, lobby_id) == 2);
    }

    {
        pqxx::work txn(f.conn);
        REQUIRE(f.repo.remove_player(txn, lobby_id, p2));
        txn.commit();
    }

    pqxx::work check(f.conn);
    REQUIRE(f.repo.player_count(check, lobby_id) == 1);
}

// L3: stale lobby deleted when last player departs
TEST_CASE("L3 stale lobby deleted when player count reaches 0", "[integration][lobby_resilience]") {
    Fixture f;
    std::string lobby_id;
    {
        pqxx::work txn(f.conn);
        lobby_id = f.repo.create(txn, f.creator_id, 2, 8).id;
        txn.commit();
    }

    {
        pqxx::work txn(f.conn);
        REQUIRE(f.repo.remove_player(txn, lobby_id, f.creator_id));
        const auto deleted = f.repo.delete_if_empty(txn, lobby_id);
        REQUIRE(deleted);
        txn.commit();
    }

    pqxx::work check(f.conn);
    REQUIRE_FALSE(f.repo.find_by_id(check, lobby_id).has_value());
}

// Negative case for L3: delete_if_empty does nothing when players remain
TEST_CASE("L3 delete_if_empty is a no-op when players remain", "[integration][lobby_resilience]") {
    Fixture f;
    std::string lobby_id;
    {
        pqxx::work txn(f.conn);
        lobby_id = f.repo.create(txn, f.creator_id, 2, 8).id;
        txn.commit();
    }

    pqxx::work txn(f.conn);
    REQUIRE_FALSE(f.repo.delete_if_empty(txn, lobby_id));
    REQUIRE(f.repo.find_by_id(txn, lobby_id).has_value());
}

// L5: rejoin is allowed after leave (no stale duplicate guard)
TEST_CASE("L5 player can rejoin lobby after leaving", "[integration][lobby_resilience]") {
    Fixture f;
    const auto p2 = f.insert_player("player2", "gh_p2_rejoin");

    std::string lobby_id;
    {
        pqxx::work txn(f.conn);
        lobby_id = f.repo.create(txn, f.creator_id, 2, 8).id;
        f.repo.add_player(txn, lobby_id, p2);
        txn.commit();
    }

    {
        pqxx::work txn(f.conn);
        REQUIRE(f.repo.remove_player(txn, lobby_id, p2));
        txn.commit();
    }

    pqxx::work txn(f.conn);
    const auto result = f.repo.add_player(txn, lobby_id, p2);
    REQUIRE(result.has_value());
}
