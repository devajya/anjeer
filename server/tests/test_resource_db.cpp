#include <catch2/catch_test_macros.hpp>

#include "server/db.h"

#include <pqxx/pqxx>
#include <stdexcept>

using namespace anjeer::server;

// ── Class 2: DbPool connection-leak tests ────────────────────────────────────

namespace {
struct DBFixture {
    DBFixture() : pool(TEST_DB_CONN, 2) {
        DbMigrator migrator(conn, TEST_MIGRATIONS_DIR);
        migrator.run();
    }
    pqxx::connection conn{ TEST_DB_CONN };
    DbPool pool;
};
} // namespace

// RAII: handle destructor must return the connection to the pool.
TEST_CASE("DbPool handle returns connection on destruction", "[resource][db]") {
    DBFixture f;
    REQUIRE(f.pool.available_count() == 2);
    {
        auto h = f.pool.acquire();
        REQUIRE(f.pool.available_count() == 1);
    }
    REQUIRE(f.pool.available_count() == 2);
}

// Exception safety: connection is returned even when the transaction throws.
TEST_CASE("DbPool returns connection when transaction throws", "[resource][db]") {
    DBFixture f;
    REQUIRE(f.pool.available_count() == 2);
    try {
        auto h = f.pool.acquire();
        pqxx::work txn(h.get());
        txn.exec("SELECT intentional_error_column_does_not_exist");
        txn.commit();
    } catch (const std::exception&) {
        // swallow
    }
    REQUIRE(f.pool.available_count() == 2);
}

// All handles must be releasable one by one — no deadlock on sequential acquire+release.
TEST_CASE("DbPool all connections releasable sequentially", "[resource][db]") {
    DBFixture f;
    {
        auto h1 = f.pool.acquire();
        REQUIRE(f.pool.available_count() == 1);
        auto h2 = f.pool.acquire();
        REQUIRE(f.pool.available_count() == 0);
    }
    REQUIRE(f.pool.available_count() == 2);
}

// Move semantics: move-constructed handle still returns connection on destruction.
TEST_CASE("DbPool moved handle returns connection on destruction", "[resource][db]") {
    DBFixture f;
    REQUIRE(f.pool.available_count() == 2);
    {
        auto h = f.pool.acquire();
        auto h2 = std::move(h);
        REQUIRE(f.pool.available_count() == 1);
    }
    REQUIRE(f.pool.available_count() == 2);
}
