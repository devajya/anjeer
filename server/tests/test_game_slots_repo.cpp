#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

#include <pqxx/pqxx>

#include "server/db.h"
#include "server/game_slots_repo.h"

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
        txn.exec("TRUNCATE TABLE game_slots CASCADE");
        txn.exec("TRUNCATE TABLE game_sessions CASCADE");
        txn.exec("TRUNCATE TABLE lobbies CASCADE");
        txn.exec("TRUNCATE TABLE players RESTART IDENTITY CASCADE");

        const auto rp = txn.exec_params(
            "INSERT INTO players (username, oauth_provider, oauth_id) "
            "VALUES ($1,$2,$3) RETURNING id",
            "slots_test_player", "github", "gh_slots_1"
        );
        player_id = rp[0][0].as<int64_t>();

        const auto rl = txn.exec_params(
            "INSERT INTO lobbies (code, creator_id, status) "
            "VALUES ($1,$2,'waiting') RETURNING id",
            "SLOTST", player_id
        );
        const std::string lobby_id = rl[0][0].as<std::string>();

        const auto rs = txn.exec_params(
            "INSERT INTO game_sessions (lobby_id, status) "
            "VALUES ($1,'active') RETURNING id",
            lobby_id
        );
        session_id = rs[0][0].as<std::string>();

        txn.commit();
    }

    pqxx::connection conn{ TEST_DB_CONN };
    DbPool           pool;
    GameSlotsRepo    repo;
    int64_t          player_id;
    std::string      session_id;
};

} // namespace

// ---------------------------------------------------------------------------
// upsert_active
// ---------------------------------------------------------------------------

TEST_CASE("GameSlotsRepo: upsert_active creates row with status=active", "[integration][game_slots_repo]") {
    Fixture f;
    f.repo.upsert_active(f.session_id, f.player_id, 0);
    std::this_thread::sleep_for(50ms);

    pqxx::work txn(f.conn);
    const auto r = txn.exec_params(
        "SELECT status, player_id FROM game_slots "
        "WHERE session_id = $1 AND slot_index = 0",
        f.session_id
    );
    txn.commit();

    REQUIRE(r.size() == 1);
    REQUIRE(r[0][0].as<std::string>() == "active");
    REQUIRE(r[0][1].as<int64_t>() == f.player_id);
}

TEST_CASE("GameSlotsRepo: upsert_active on conflict resets status to active", "[integration][game_slots_repo]") {
    Fixture f;
    f.repo.upsert_active(f.session_id, f.player_id, 0);
    std::this_thread::sleep_for(50ms);

    // Manually set disconnected then re-upsert.
    {
        pqxx::work txn(f.conn);
        txn.exec_params(
            "UPDATE game_slots SET status='disconnected', disconnected_at=NOW() "
            "WHERE session_id=$1 AND slot_index=0",
            f.session_id
        );
        txn.commit();
    }

    f.repo.upsert_active(f.session_id, f.player_id, 0);
    std::this_thread::sleep_for(50ms);

    pqxx::work txn(f.conn);
    const auto r = txn.exec_params(
        "SELECT status, disconnected_at FROM game_slots "
        "WHERE session_id=$1 AND slot_index=0",
        f.session_id
    );
    txn.commit();

    REQUIRE(r[0][0].as<std::string>() == "active");
    REQUIRE(r[0][1].is_null());
}

// ---------------------------------------------------------------------------
// mark_disconnected
// ---------------------------------------------------------------------------

TEST_CASE("GameSlotsRepo: mark_disconnected sets status and disconnected_at", "[integration][game_slots_repo]") {
    Fixture f;
    f.repo.upsert_active(f.session_id, f.player_id, 1);
    std::this_thread::sleep_for(50ms);

    f.repo.mark_disconnected(f.session_id, 1);
    std::this_thread::sleep_for(50ms);

    pqxx::work txn(f.conn);
    const auto r = txn.exec_params(
        "SELECT status, disconnected_at FROM game_slots "
        "WHERE session_id=$1 AND slot_index=1",
        f.session_id
    );
    txn.commit();

    REQUIRE(r[0][0].as<std::string>() == "disconnected");
    REQUIRE_FALSE(r[0][1].is_null());
}

// ---------------------------------------------------------------------------
// mark_expired
// ---------------------------------------------------------------------------

TEST_CASE("GameSlotsRepo: mark_expired sets status=expired", "[integration][game_slots_repo]") {
    Fixture f;
    f.repo.upsert_active(f.session_id, f.player_id, 2);
    std::this_thread::sleep_for(50ms);

    f.repo.mark_expired(f.session_id, 2);
    std::this_thread::sleep_for(50ms);

    pqxx::work txn(f.conn);
    const auto r = txn.exec_params(
        "SELECT status FROM game_slots WHERE session_id=$1 AND slot_index=2",
        f.session_id
    );
    txn.commit();

    REQUIRE(r[0][0].as<std::string>() == "expired");
}

// ---------------------------------------------------------------------------
// set_payout
// ---------------------------------------------------------------------------

TEST_CASE("GameSlotsRepo: set_payout writes pending_payout", "[integration][game_slots_repo]") {
    Fixture f;
    f.repo.upsert_active(f.session_id, f.player_id, 3);
    std::this_thread::sleep_for(50ms);

    f.repo.set_payout(f.session_id, 3, 250);
    std::this_thread::sleep_for(50ms);

    pqxx::work txn(f.conn);
    const auto r = txn.exec_params(
        "SELECT pending_payout FROM game_slots WHERE session_id=$1 AND slot_index=3",
        f.session_id
    );
    txn.commit();

    REQUIRE(r[0][0].as<int>() == 250);
}

// ---------------------------------------------------------------------------
// mark_reattached
// ---------------------------------------------------------------------------

TEST_CASE("GameSlotsRepo: mark_reattached clears disconnected_at and sets active", "[integration][game_slots_repo]") {
    Fixture f;
    f.repo.upsert_active(f.session_id, f.player_id, 4);
    std::this_thread::sleep_for(50ms);

    f.repo.mark_disconnected(f.session_id, 4);
    std::this_thread::sleep_for(50ms);

    f.repo.mark_reattached(f.session_id, 4);
    std::this_thread::sleep_for(50ms);

    pqxx::work txn(f.conn);
    const auto r = txn.exec_params(
        "SELECT status, disconnected_at FROM game_slots "
        "WHERE session_id=$1 AND slot_index=4",
        f.session_id
    );
    txn.commit();

    REQUIRE(r[0][0].as<std::string>() == "active");
    REQUIRE(r[0][1].is_null());
}

// ---------------------------------------------------------------------------
// get_by_session
// ---------------------------------------------------------------------------

TEST_CASE("GameSlotsRepo: get_by_session returns all rows ordered by slot_index", "[integration][game_slots_repo]") {
    Fixture f;
    f.repo.upsert_active(f.session_id, f.player_id, 0);
    f.repo.upsert_active(f.session_id, f.player_id, 1);
    std::this_thread::sleep_for(50ms);

    const auto rows = f.repo.get_by_session(f.session_id);

    REQUIRE(rows.size() == 2);
    REQUIRE(rows[0].slot_index == 0);
    REQUIRE(rows[1].slot_index == 1);
    REQUIRE(rows[0].status == SlotStatus::active);
    REQUIRE(rows[0].player_id == f.player_id);
}
