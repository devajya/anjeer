#pragma once

// AGENT-CTX: db.h is the public façade for DbPool and DbMigrator — the
// infrastructure types that callers outside the data layer need. Files that
// issue SQL directly (player_repo.h, auth_service.h) still include <pqxx/pqxx>
// themselves because pqxx::transaction_base cannot be forward-declared given
// its template hierarchy. The intended boundary: main.cpp and HttpServer include
// only db.h; only repo/service layer headers include pqxx directly.

#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include <pqxx/pqxx>

namespace anjeer::server {

// AGENT-CTX: DbPool owns N persistent pqxx::connection objects and hands them
// out as RAII Handles. Handles are move-only; the connection is returned to the
// pool on Handle destruction. acquire() blocks until a connection is free.
//
// Thread safety: acquire/release are mutex-guarded; the pool itself is
// thread-safe. Individual pqxx::connection objects are NOT thread-safe —
// callers must not share a Handle across threads. Each Crow handler thread
// gets its own Handle from acquire().
//
// pool_size is from ServerConfig::DbConfig. For Slices 5-6 (single Crow
// thread pool on one machine), 4 is sufficient. Revisit in Slice 12 when
// the DB writer thread is added.
class DbPool {
public:
    // AGENT-CTX: Handle is move-only. Moving transfers ownership; the moved-from
    // Handle's pool_ is set to nullptr so its destructor is a no-op. Do not hold
    // a Handle across an await point or a thread boundary.
    class Handle {
    public:
        Handle(Handle&& other) noexcept;
        Handle& operator=(Handle&& other) noexcept;
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
        ~Handle();

        pqxx::connection& get() { return *conn_; }

    private:
        friend class DbPool;
        Handle(DbPool* pool, pqxx::connection* conn) noexcept
            : pool_(pool), conn_(conn) {}

        DbPool*           pool_;
        pqxx::connection* conn_;
    };

    explicit DbPool(std::string_view conn_str, int size);
    ~DbPool() = default;

    DbPool(const DbPool&)            = delete;
    DbPool& operator=(const DbPool&) = delete;

    // Blocks until a connection is available.
    Handle acquire();

private:
    void release(pqxx::connection* conn);

    std::vector<std::unique_ptr<pqxx::connection>> owned_;
    std::queue<pqxx::connection*>                  available_;
    std::mutex                                     mu_;
    std::condition_variable                        cv_;
};

// AGENT-CTX: DbMigrator applies SQL migration files from migrations_dir in
// ascending version order (filename prefix NNN). It bootstraps a
// schema_migrations table on first run and is idempotent — already-applied
// versions are skipped. Each migration runs in its own transaction; a failure
// leaves previously-applied migrations committed and the failed one rolled back.
//
// Call run() once at server startup before opening the connection pool. The
// connection passed here is used solely for migration and is not shared with
// the pool — pass a dedicated connection constructed from the same conn_str.
class DbMigrator {
public:
    explicit DbMigrator(pqxx::connection& conn, std::string_view migrations_dir);
    void run();

private:
    pqxx::connection& conn_;
    std::string       migrations_dir_;
};

} // namespace anjeer::server
