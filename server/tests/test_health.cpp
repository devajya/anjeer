#include <catch2/catch_test_macros.hpp>

#include "server/db.h"

#include <pqxx/pqxx>
#include <stdexcept>

using namespace anjeer::server;

// Mirrors the health endpoint logic: acquire connection + SELECT 1.

TEST_CASE("GET /health returns 200 when DB is reachable", "[health][integration]") {
    pqxx::connection setup_conn(TEST_DB_CONN);
    DbMigrator migrator(setup_conn, TEST_MIGRATIONS_DIR);
    migrator.run();
    DbPool pool(TEST_DB_CONN, 1);

    REQUIRE_NOTHROW([&]() {
        auto h = pool.acquire();
        pqxx::nontransaction ntxn(h.get());
        ntxn.exec("SELECT 1");
    }());
}

TEST_CASE("GET /health returns 503 when DB pool fails SELECT 1", "[health][integration]") {
    // A connection to a non-existent DB host must throw — the health handler
    // catches this and returns 503. We verify the throwing behaviour here.
    REQUIRE_THROWS([&]() {
        DbPool bad("postgresql://localhost:9999/does_not_exist", 1);
        auto h = bad.acquire();
        pqxx::nontransaction ntxn(h.get());
        ntxn.exec("SELECT 1");
    }());
}
