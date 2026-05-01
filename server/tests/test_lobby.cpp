#include <catch2/catch_test_macros.hpp>

#include "server/db.h"
#include "server/lobby_repo.h"

namespace {

struct TestDbFixture {
    explicit TestDbFixture() : conn(TEST_DB_CONN) {
        anjeer::server::DbMigrator migrator(conn, TEST_MIGRATIONS_DIR);
        migrator.run();
        {
            pqxx::work txn(conn);
            txn.exec("TRUNCATE TABLE lobbies CASCADE");
            txn.exec("TRUNCATE TABLE players RESTART IDENTITY CASCADE");
            txn.commit();
        }
        {
            pqxx::work txn(conn);
            const auto r = txn.exec_params(
                "INSERT INTO players (username, oauth_provider, oauth_id) "
                "VALUES ($1, $2, $3) RETURNING id",
                "test_owner", "github", "gh_owner_1"
            );
            owner_id = r[0][0].as<int64_t>();
            txn.commit();
        }
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
    int64_t                   owner_id;
};

} // namespace

// ---------------------------------------------------------------------------
// AC1 — create lobby, get code
// ---------------------------------------------------------------------------

TEST_CASE("LobbyRepo::create returns lobby with unique 6-char code", "[integration][lobby_repo]") {
    TestDbFixture f;
    pqxx::work txn(f.conn);
    const auto lobby = f.repo.create(txn, f.owner_id, 2, 8);
    txn.commit();

    REQUIRE(lobby.code.size() == 6);
    REQUIRE(lobby.creator_id  == f.owner_id);
    REQUIRE(lobby.status      == anjeer::server::LobbyStatus::Waiting);
    REQUIRE(lobby.min_players == 2);
    REQUIRE(lobby.max_players == 8);
    REQUIRE_FALSE(lobby.id.empty());
}

// ---------------------------------------------------------------------------
// AC2 — list open lobbies with counts
// ---------------------------------------------------------------------------

TEST_CASE("LobbyRepo::list_waiting returns only waiting lobbies", "[integration][lobby_repo]") {
    TestDbFixture f;
    // Create and immediately start a lobby so it leaves the 'waiting' slot,
    // then create the lobby that should appear in list_waiting.
    {
        pqxx::work txn(f.conn);
        const auto lobby = f.repo.create(txn, f.owner_id, 2, 8);
        f.repo.transition_status(txn, lobby.id,
            anjeer::server::LobbyStatus::Waiting,
            anjeer::server::LobbyStatus::Starting);
        txn.commit();
    }
    {
        pqxx::work txn(f.conn);
        f.repo.create(txn, f.owner_id, 2, 8);
        txn.commit();
    }

    pqxx::work txn(f.conn);
    const auto views = f.repo.list_waiting(txn);
    REQUIRE(views.size() == 1);
    REQUIRE(views[0].lobby.status == anjeer::server::LobbyStatus::Waiting);
}

TEST_CASE("LobbyRepo::list_waiting includes player count", "[integration][lobby_repo]") {
    TestDbFixture f;
    std::string lobby_id;
    {
        pqxx::work txn(f.conn);
        const auto lobby = f.repo.create(txn, f.owner_id, 2, 8);
        lobby_id = lobby.id;
        f.repo.add_player(txn, lobby_id, f.owner_id);
        txn.commit();
    }

    pqxx::work txn(f.conn);
    const auto views = f.repo.list_waiting(txn);
    REQUIRE(views.size() == 1);
    REQUIRE(views[0].player_count == 1);
}

// ---------------------------------------------------------------------------
// AC3 — join rejects
// ---------------------------------------------------------------------------

TEST_CASE("LobbyRepo::add_player is idempotent for duplicate join", "[integration][lobby_repo]") {
    TestDbFixture f;
    std::string lobby_id;
    {
        pqxx::work txn(f.conn);
        const auto lobby = f.repo.create(txn, f.owner_id, 2, 8);
        lobby_id = lobby.id;
        txn.commit();
    }

    // AGENT-CTX: create() now auto-inserts the creator row. Re-joining the same
    // player must return the existing joined_at (idempotent), not nullopt.
    pqxx::work txn(f.conn);
    const auto result = f.repo.add_player(txn, lobby_id, f.owner_id);
    REQUIRE(result.has_value());
}

TEST_CASE("LobbyRepo::add_player returns false when lobby full", "[integration][lobby_repo]") {
    TestDbFixture f;
    const auto player2_id = f.insert_player("player2", "gh_player2");

    std::string lobby_id;
    {
        pqxx::work txn(f.conn);
        const auto lobby = f.repo.create(txn, f.owner_id, 1, 1);
        lobby_id = lobby.id;
        f.repo.add_player(txn, lobby_id, f.owner_id);
        txn.commit();
    }

    pqxx::work txn(f.conn);
    REQUIRE_FALSE(f.repo.add_player(txn, lobby_id, player2_id));
}

TEST_CASE("LobbyRepo::add_player returns false when not waiting", "[integration][lobby_repo]") {
    TestDbFixture f;
    std::string lobby_id;
    {
        pqxx::work txn(f.conn);
        const auto lobby = f.repo.create(txn, f.owner_id, 2, 8);
        lobby_id = lobby.id;
        f.repo.transition_status(txn, lobby_id,
            anjeer::server::LobbyStatus::Waiting,
            anjeer::server::LobbyStatus::Starting);
        txn.commit();
    }

    pqxx::work txn(f.conn);
    REQUIRE_FALSE(f.repo.add_player(txn, lobby_id, f.owner_id));
}

// ---------------------------------------------------------------------------
// AC4 — status transitions
// ---------------------------------------------------------------------------

TEST_CASE("LobbyRepo::transition_status from waiting to starting succeeds", "[integration][lobby_repo]") {
    TestDbFixture f;
    std::string lobby_id;
    {
        pqxx::work txn(f.conn);
        const auto lobby = f.repo.create(txn, f.owner_id, 2, 8);
        lobby_id = lobby.id;
        txn.commit();
    }

    {
        pqxx::work txn(f.conn);
        REQUIRE(f.repo.transition_status(txn, lobby_id,
            anjeer::server::LobbyStatus::Waiting,
            anjeer::server::LobbyStatus::Starting));
        txn.commit();
    }

    pqxx::work txn(f.conn);
    const auto found = f.repo.find_by_id(txn, lobby_id);
    REQUIRE(found.has_value());
    REQUIRE(found->status == anjeer::server::LobbyStatus::Starting);
}

TEST_CASE("LobbyRepo::transition_status returns false if status mismatch", "[integration][lobby_repo]") {
    TestDbFixture f;
    std::string lobby_id;
    {
        pqxx::work txn(f.conn);
        const auto lobby = f.repo.create(txn, f.owner_id, 2, 8);
        lobby_id = lobby.id;
        txn.commit();
    }

    pqxx::work txn(f.conn);
    // Lobby is 'waiting'; trying to move from 'starting' must fail.
    REQUIRE_FALSE(f.repo.transition_status(txn, lobby_id,
        anjeer::server::LobbyStatus::Starting,
        anjeer::server::LobbyStatus::InGame));
}

// ---------------------------------------------------------------------------
// AC6 — persistence
// ---------------------------------------------------------------------------

TEST_CASE("LobbyRepo::find_by_code returns lobby after create", "[integration][lobby_repo]") {
    TestDbFixture f;
    std::string code;
    {
        pqxx::work txn(f.conn);
        const auto lobby = f.repo.create(txn, f.owner_id, 2, 8);
        code = lobby.code;
        txn.commit();
    }

    pqxx::work txn(f.conn);
    const auto found = f.repo.find_by_code(txn, code);
    REQUIRE(found.has_value());
    REQUIRE(found->code     == code);
    REQUIRE(found->creator_id == f.owner_id);
    REQUIRE(found->status   == anjeer::server::LobbyStatus::Waiting);
}

TEST_CASE("LobbyRepo::find_by_id returns nullopt for unknown id", "[integration][lobby_repo]") {
    TestDbFixture f;
    pqxx::work txn(f.conn);
    const auto found = f.repo.find_by_id(txn, "00000000-0000-0000-0000-000000000000");
    REQUIRE_FALSE(found.has_value());
}
