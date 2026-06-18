#include "server/db.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace anjeer::server {

// ---------------------------------------------------------------------------
// DbPool::Handle
// ---------------------------------------------------------------------------

DbPool::Handle::Handle(Handle&& other) noexcept
    : pool_(other.pool_), conn_(other.conn_) {
    other.pool_ = nullptr;
    other.conn_ = nullptr;
}

DbPool::Handle& DbPool::Handle::operator=(Handle&& other) noexcept {
    if (this != &other) {
        if (pool_) pool_->release(conn_);
        pool_       = other.pool_;
        conn_       = other.conn_;
        other.pool_ = nullptr;
        other.conn_ = nullptr;
    }
    return *this;
}

DbPool::Handle::~Handle() {
    if (pool_) pool_->release(conn_);
}

// ---------------------------------------------------------------------------
// DbPool
// ---------------------------------------------------------------------------

DbPool::DbPool(std::string_view conn_str, int size) {
    // AGENT-CTX: All connections are opened eagerly at construction so that
    // startup failures (bad connection string, DB unreachable) surface
    // immediately rather than on the first request. pqxx::connection throws
    // pqxx::broken_connection if it cannot connect; let it propagate.
    owned_.reserve(static_cast<std::size_t>(size));
    for (int i = 0; i < size; ++i) {
        owned_.push_back(std::make_unique<pqxx::connection>(std::string(conn_str)));
        available_.push(owned_.back().get());
    }
}

DbPool::Handle DbPool::acquire() {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this] { return !available_.empty(); });
    pqxx::connection* conn = available_.front();
    available_.pop();
    return Handle(this, conn);
}

void DbPool::release(pqxx::connection* conn) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        available_.push(conn);
    }
    cv_.notify_one();
}

// ---------------------------------------------------------------------------
// DbMigrator
// ---------------------------------------------------------------------------

DbMigrator::DbMigrator(pqxx::connection& conn, std::string_view migrations_dir)
    : conn_(conn), migrations_dir_(migrations_dir) {}

void DbMigrator::run() {
    namespace fs = std::filesystem;

    // AGENT-CTX: schema_migrations bootstraps itself — the table must exist
    // before we can query it for applied versions. IF NOT EXISTS makes this
    // idempotent regardless of how many times run() is called.
    {
        pqxx::work txn(conn_);
        txn.exec(
            "CREATE TABLE IF NOT EXISTS schema_migrations ("
            "  version    INT         PRIMARY KEY,"
            "  applied_at TIMESTAMPTZ NOT NULL DEFAULT now()"
            ")");
        txn.commit();
    }

    // Collect already-applied versions.
    std::set<int> applied;
    {
        pqxx::work txn(conn_);
        for (const auto& row : txn.exec("SELECT version FROM schema_migrations")) {
            applied.insert(row[0].as<int>());
        }
        txn.commit();
    }

    // Collect migration files: filename must start with digits followed by '_'.
    // AGENT-CTX: We read the entire migrations_dir, not just a hardcoded list,
    // so future slices can drop a new SQL file and have it applied automatically.
    // Version number is the leading digit sequence before the first '_' character.
    // Non-conforming filenames (no leading digits, no underscore) are silently
    // skipped — this allows placing README or other docs in the same directory.
    std::vector<std::pair<int, fs::path>> pending;
    if (!fs::exists(migrations_dir_)) {
        throw std::runtime_error("migrations_dir does not exist: " + migrations_dir_);
    }
    for (const auto& entry : fs::directory_iterator(migrations_dir_)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".sql") continue;
        const std::string stem = entry.path().stem().string();
        const auto        sep  = stem.find('_');
        if (sep == std::string::npos || sep == 0) continue;
        try {
            int version = std::stoi(stem.substr(0, sep));
            if (!applied.count(version)) {
                pending.emplace_back(version, entry.path());
            }
        } catch (...) {
            // stoi failed — filename doesn't start with an integer; skip.
        }
    }
    std::sort(pending.begin(), pending.end());

    std::cout << "[migrator] " << pending.size() << " pending migration(s)\n";

    for (const auto& [version, path] : pending) {
        std::ifstream file(path);
        if (!file.is_open()) {
            throw std::runtime_error("Cannot open migration file: " + path.string());
        }
        const std::string sql(
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>{}
        );

        std::cout << "[migrator] applying v" << version << ": " << path.filename().string() << "\n";
        pqxx::work txn(conn_);
        txn.exec(sql);
        txn.exec_params(
            "INSERT INTO schema_migrations (version) VALUES ($1)", version);
        txn.commit();
        std::cout << "[migrator] v" << version << " applied\n";
    }
}

} // namespace anjeer::server
