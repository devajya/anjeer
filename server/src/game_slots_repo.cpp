#include "server/game_slots_repo.h"

#include <sstream>
#include <stdexcept>
#include <thread>

#include <pqxx/pqxx>

namespace anjeer::server {

GameSlotsRepo::GameSlotsRepo(DbPool& pool) : pool_(pool) {}

// ── Async writes ───────────────────────────────────────────────────────────────

void GameSlotsRepo::upsert_active(const std::string& session_id,
                                  int64_t player_id,
                                  int slot_index) {
    DbPool* p = &pool_;
    std::thread([p, session_id, player_id, slot_index]() {
        auto handle = p->acquire();
        pqxx::work txn(handle.get());
        txn.exec_params(
            "INSERT INTO game_slots (session_id, player_id, slot_index, status, updated_at) "
            "VALUES ($1, $2, $3, 'active', NOW()) "
            "ON CONFLICT (session_id, slot_index) DO UPDATE "
            "SET player_id = $2, status = 'active', disconnected_at = NULL, updated_at = NOW()",
            session_id, player_id, slot_index
        );
        txn.commit();
    }).detach();
}

void GameSlotsRepo::mark_disconnected(const std::string& session_id, int slot_index) {
    DbPool* p = &pool_;
    std::thread([p, session_id, slot_index]() {
        auto handle = p->acquire();
        pqxx::work txn(handle.get());
        txn.exec_params(
            "UPDATE game_slots SET status = 'disconnected', disconnected_at = NOW(), "
            "updated_at = NOW() WHERE session_id = $1 AND slot_index = $2",
            session_id, slot_index
        );
        txn.commit();
    }).detach();
}

void GameSlotsRepo::mark_expired(const std::string& session_id, int slot_index) {
    DbPool* p = &pool_;
    std::thread([p, session_id, slot_index]() {
        auto handle = p->acquire();
        pqxx::work txn(handle.get());
        txn.exec_params(
            "UPDATE game_slots SET status = 'expired', updated_at = NOW() "
            "WHERE session_id = $1 AND slot_index = $2",
            session_id, slot_index
        );
        txn.commit();
    }).detach();
}

void GameSlotsRepo::set_payout(const std::string& session_id, int slot_index, int payout) {
    DbPool* p = &pool_;
    std::thread([p, session_id, slot_index, payout]() {
        auto handle = p->acquire();
        pqxx::work txn(handle.get());
        txn.exec_params(
            "UPDATE game_slots SET pending_payout = $3, updated_at = NOW() "
            "WHERE session_id = $1 AND slot_index = $2",
            session_id, slot_index, payout
        );
        txn.commit();
    }).detach();
}

void GameSlotsRepo::mark_reattached(const std::string& session_id, int slot_index) {
    DbPool* p = &pool_;
    std::thread([p, session_id, slot_index]() {
        auto handle = p->acquire();
        pqxx::work txn(handle.get());
        txn.exec_params(
            "UPDATE game_slots SET status = 'active', disconnected_at = NULL, "
            "updated_at = NOW() WHERE session_id = $1 AND slot_index = $2",
            session_id, slot_index
        );
        txn.commit();
    }).detach();
}

// ── Sync read ──────────────────────────────────────────────────────────────────

std::vector<GameSlotRecord> GameSlotsRepo::get_by_session(const std::string& session_id) {
    auto handle = pool_.acquire();
    pqxx::work txn(handle.get());
    const auto rows = txn.exec_params(
        "SELECT session_id, player_id, slot_index, status, pending_payout, disconnected_at "
        "FROM game_slots WHERE session_id = $1 ORDER BY slot_index",
        session_id
    );
    txn.commit();

    std::vector<GameSlotRecord> result;
    result.reserve(rows.size());
    for (const auto& row : rows) {
        GameSlotRecord rec;
        rec.session_id     = row[0].as<std::string>();
        rec.player_id      = row[1].as<int64_t>();
        rec.slot_index     = row[2].as<int>();
        rec.status         = parse_status(row[3].as<std::string>());
        rec.pending_payout = row[4].as<int>();
        if (!row[5].is_null())
            rec.disconnected_at = parse_ts(row[5].as<std::string>());
        result.push_back(std::move(rec));
    }
    return result;
}

// ── Private helpers ────────────────────────────────────────────────────────────

std::chrono::system_clock::time_point GameSlotsRepo::parse_ts(const std::string& s) {
    struct tm tm_val = {};
    std::istringstream ss(s);
    ss >> std::get_time(&tm_val, "%Y-%m-%d %H:%M:%S");
    auto tt = std::mktime(&tm_val);
    return std::chrono::system_clock::from_time_t(tt);
}

SlotStatus GameSlotsRepo::parse_status(const std::string& s) {
    if (s == "active")       return SlotStatus::active;
    if (s == "disconnected") return SlotStatus::disconnected;
    if (s == "expired")      return SlotStatus::expired;
    throw std::runtime_error("unknown slot status: " + s);
}

} // namespace anjeer::server
