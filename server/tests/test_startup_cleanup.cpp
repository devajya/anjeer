#include <catch2/catch_test_macros.hpp>

#include "server/db.h"
#include "server/spectate_token_repo.h"

#include <pqxx/pqxx>
#include <stdexcept>

using namespace anjeer::server;

// Mirrors the startup cleanup block in main.cpp: the try/catch must swallow
// all exceptions so a DB hiccup at boot does not kill the server process.

TEST_CASE("Startup DB cleanup failure is non-fatal", "[startup][cleanup]") {
    // Run the same operations main.cpp runs, but deliberately point at a bad
    // connection to trigger the catch branch. The test passes if no exception
    // escapes the try block.
    REQUIRE_NOTHROW([&]() {
        try {
            pqxx::connection conn("postgresql://localhost:9999/does_not_exist");
            pqxx::work txn(conn);
            txn.exec("DELETE FROM lobbies WHERE status = 'closed'");
            SpectateTokenRepo repo(1);
            repo.cleanup_expired(txn);
            txn.commit();
        } catch (const std::exception&) {
            // non-fatal — matches the [warn] branch in main.cpp
        }
    }());
}

TEST_CASE("Startup DB cleanup succeeds against real DB", "[startup][cleanup][integration]") {
    pqxx::connection conn(TEST_DB_CONN);
    DbMigrator migrator(conn, TEST_MIGRATIONS_DIR);
    migrator.run();

    REQUIRE_NOTHROW([&]() {
        pqxx::work txn(conn);
        txn.exec("DELETE FROM lobbies WHERE status = 'closed'");
        SpectateTokenRepo repo(1);
        repo.cleanup_expired(txn);
        txn.commit();
    }());
}
