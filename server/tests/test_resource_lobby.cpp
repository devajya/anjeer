#include <catch2/catch_test_macros.hpp>

#include "server/db.h"
#include "server/lobby_repo.h"

#include <pqxx/pqxx>

using namespace anjeer::server;

// ── Class 3: lobby accumulation tests ────────────────────────────────────────

namespace {
struct LobbyFixture {
    LobbyFixture() : conn(TEST_DB_CONN) {
        DbMigrator migrator(conn, TEST_MIGRATIONS_DIR);
        migrator.run();
        pqxx::work txn(conn);
        txn.exec("TRUNCATE TABLE lobbies CASCADE");
        txn.exec("TRUNCATE TABLE players RESTART IDENTITY CASCADE");
        const auto r = txn.exec_params(
            "INSERT INTO players (username, oauth_provider, oauth_id) "
            "VALUES ($1,$2,$3) RETURNING id",
            "res_lobby_player", "github", "gh_reslob_1"
        );
        owner_id = r[0][0].as<int64_t>();
        txn.commit();
    }

    pqxx::connection conn;
    LobbyRepo        repo;
    int64_t          owner_id;
};
} // namespace

// delete_if_empty removes an empty lobby — no accumulation of empty rows.
// create() always inserts the creator into lobby_players, so we must remove
// them first to get a genuinely empty lobby.
TEST_CASE("LobbyRepo delete_if_empty removes lobby with no players", "[resource][lobby]") {
    LobbyFixture f;
    std::string lobby_id;
    {
        pqxx::work txn(f.conn);
        lobby_id = f.repo.create(txn, {f.owner_id, 2, 8}).id;
        txn.exec_params("DELETE FROM lobby_players WHERE lobby_id = $1", lobby_id);
        txn.commit();
    }
    {
        pqxx::work txn(f.conn);
        bool removed = f.repo.delete_if_empty(txn, lobby_id);
        txn.commit();
        REQUIRE(removed);
    }
    {
        pqxx::work txn(f.conn);
        auto found = f.repo.find_by_id(txn, lobby_id);
        txn.commit();
        REQUIRE_FALSE(found.has_value());
    }
}

// delete_if_empty preserves a lobby that still has a player.
// create() already adds the creator to lobby_players, so a freshly created
// lobby has one player and delete_if_empty must leave it alone.
TEST_CASE("LobbyRepo delete_if_empty leaves lobby with players", "[resource][lobby]") {
    LobbyFixture f;
    std::string lobby_id;
    {
        pqxx::work txn(f.conn);
        lobby_id = f.repo.create(txn, {f.owner_id, 2, 8}).id;
        txn.commit();
    }
    {
        pqxx::work txn(f.conn);
        bool removed = f.repo.delete_if_empty(txn, lobby_id);
        txn.commit();
        REQUIRE_FALSE(removed);
    }
    {
        pqxx::work txn(f.conn);
        auto found = f.repo.find_by_id(txn, lobby_id);
        txn.commit();
        REQUIRE(found.has_value());
    }
}

// Closed lobbies are pruned at startup — validated by the startup cleanup in main.cpp.
// This test ensures the DELETE FROM lobbies WHERE status='closed' path runs cleanly
// against the schema (not that it removes any rows — the fixture has none).
TEST_CASE("Startup cleanup DELETE closed lobbies does not error on empty table", "[resource][lobby]") {
    LobbyFixture f;
    pqxx::work txn(f.conn);
    REQUIRE_NOTHROW(txn.exec("DELETE FROM lobbies WHERE status = 'closed'"));
    txn.commit();
}

// Stub: lobby timeout / stuck-in-starting pruning is not yet implemented.
// This test documents the expected future behaviour and fails until it is built.
TEST_CASE("LobbyRepo prunes lobbies stuck in starting status — STUB", "[resource][lobby][!shouldfail]") {
    LobbyFixture f;
    std::string lobby_id;
    {
        pqxx::work txn(f.conn);
        auto lobby = f.repo.create(txn, {f.owner_id, 2, 8});
        lobby_id = lobby.id;
        f.repo.transition_status(txn, lobby_id, LobbyStatus::Waiting, LobbyStatus::Starting);
        // backdate created_at by 10 minutes to simulate a stuck lobby
        txn.exec_params(
            "UPDATE lobbies SET created_at = NOW() - INTERVAL '10 minutes' WHERE id = $1",
            lobby_id
        );
        txn.commit();
    }
    // Once the pruner is built, it should close or delete this lobby.
    // This assertion documents the desired future state — it currently fails
    // (lobby is still there because no pruner exists) and is marked [!shouldfail].
    pqxx::work txn(f.conn);
    auto found = f.repo.find_by_id(txn, lobby_id);
    txn.commit();
    // When implemented: the pruner should have removed or closed this lobby.
    REQUIRE_FALSE(found.has_value());
}
