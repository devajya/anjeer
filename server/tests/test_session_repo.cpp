#include <catch2/catch_test_macros.hpp>

#include "server/db.h"
#include "server/lobby_repo.h"
#include "server/session_repo.h"

namespace {

struct Fixture {
    explicit Fixture() : conn(TEST_DB_CONN) {
        anjeer::server::DbMigrator migrator(conn, TEST_MIGRATIONS_DIR);
        migrator.run();
        pqxx::work txn(conn);
        txn.exec("TRUNCATE TABLE game_sessions CASCADE");
        txn.exec("TRUNCATE TABLE lobbies CASCADE");
        txn.exec("TRUNCATE TABLE players RESTART IDENTITY CASCADE");

        const auto r = txn.exec_params(
            "INSERT INTO players (username, oauth_provider, oauth_id) "
            "VALUES ($1, $2, $3) RETURNING id",
            "test_player", "github", "gh_test_1"
        );
        player_id = r[0][0].as<int64_t>();

        const auto lobby_r = txn.exec_params(
            "INSERT INTO lobbies (code, creator_id, min_players, max_players) "
            "VALUES ($1, $2, $3, $4) RETURNING id",
            "TSTLBY", player_id, 2, 8
        );
        lobby_id = lobby_r[0][0].as<std::string>();

        txn.commit();
    }

    pqxx::connection          conn;
    anjeer::server::SessionRepo repo;
    int64_t                   player_id;
    std::string               lobby_id;
};

} // namespace

TEST_CASE("SessionRepo::create_session inserts active row", "[integration][session_repo]") {
    Fixture f;
    pqxx::work txn(f.conn);
    const auto session_id = f.repo.create_session(txn, f.lobby_id);
    txn.commit();

    REQUIRE_FALSE(session_id.empty());

    pqxx::work check(f.conn);
    const auto r = check.exec_params(
        "SELECT status, rounds_played FROM game_sessions WHERE id = $1", session_id
    );
    REQUIRE(r.size() == 1);
    REQUIRE(r[0]["status"].as<std::string>() == "active");
    REQUIRE(r[0]["rounds_played"].as<int>() == 0);
}

TEST_CASE("SessionRepo::create_round and end_round", "[integration][session_repo]") {
    Fixture f;
    std::string session_id, round_id;
    {
        pqxx::work txn(f.conn);
        session_id = f.repo.create_session(txn, f.lobby_id);
        round_id   = f.repo.create_round(txn, session_id, 1, "clubs", 40);
        txn.commit();
    }

    REQUIRE_FALSE(round_id.empty());

    {
        pqxx::work txn(f.conn);
        const auto r = txn.exec_params(
            "SELECT round_number, goal_suit, pot, ended_at FROM rounds WHERE id = $1",
            round_id
        );
        REQUIRE(r.size() == 1);
        REQUIRE(r[0]["round_number"].as<int>() == 1);
        REQUIRE(r[0]["goal_suit"].as<std::string>() == "clubs");
        REQUIRE(r[0]["pot"].as<int>() == 40);
        REQUIRE(r[0]["ended_at"].is_null());
    }

    {
        pqxx::work txn(f.conn);
        f.repo.end_round(txn, round_id);
        txn.commit();
    }

    pqxx::work txn(f.conn);
    const auto r = txn.exec_params("SELECT ended_at FROM rounds WHERE id = $1", round_id);
    REQUIRE_FALSE(r[0]["ended_at"].is_null());
}

TEST_CASE("SessionRepo::end_session marks ended with rounds_played", "[integration][session_repo]") {
    Fixture f;
    std::string session_id;
    {
        pqxx::work txn(f.conn);
        session_id = f.repo.create_session(txn, f.lobby_id);
        txn.commit();
    }

    {
        pqxx::work txn(f.conn);
        f.repo.end_session(txn, session_id, 3);
        txn.commit();
    }

    pqxx::work txn(f.conn);
    const auto r = txn.exec_params(
        "SELECT status, rounds_played, ended_at FROM game_sessions WHERE id = $1", session_id
    );
    REQUIRE(r[0]["status"].as<std::string>() == "ended");
    REQUIRE(r[0]["rounds_played"].as<int>() == 3);
    REQUIRE_FALSE(r[0]["ended_at"].is_null());
}

TEST_CASE("SessionRepo::write_error inserts session_errors row", "[integration][session_repo]") {
    Fixture f;
    std::string session_id;
    {
        pqxx::work txn(f.conn);
        session_id = f.repo.create_session(txn, f.lobby_id);
        txn.commit();
    }

    {
        pqxx::work txn(f.conn);
        f.repo.write_error(txn, session_id, "UNHANDLED_EXCEPTION", "test error",
                           nlohmann::json{{"round", 2}});
        txn.commit();
    }

    pqxx::work txn(f.conn);
    const auto r = txn.exec_params(
        "SELECT error_type, message FROM session_errors WHERE session_id = $1", session_id
    );
    REQUIRE(r.size() == 1);
    REQUIRE(r[0]["error_type"].as<std::string>() == "UNHANDLED_EXCEPTION");
    REQUIRE(r[0]["message"].as<std::string>() == "test error");
}
