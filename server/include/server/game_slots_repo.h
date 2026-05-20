#pragma once

// AGENT-CTX: GameSlotsRepo is the only path to the game_slots table.
// Rows track per-slot connection state during an active session, enabling
// hold-and-score: a disconnected player's pending payout is computed at
// round end and stored here even if their WS is gone.
//
// All writes are async (std::async tracked in futures_, joined in destructor).
// This keeps every caller — all on the uWS event-loop thread — non-blocking.
// The FK on session_id is safe because the
// game_sessions row is committed synchronously before any slot rows are
// written (T9 obligation).
//
// get_by_session() is the only synchronous call; it is used only at game
// end for payout reconciliation, outside the hot path.
//
// AGENT-CTX: player_id is int64_t (matches the DB BIGINT column) not
// std::string. The T5 spec interface showed std::string but the 0015
// migration uses BIGINT REFERENCES players(id), consistent with every
// other repo in this codebase.

#include <chrono>
#include <cstdint>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "server/db.h"

namespace anjeer::server {

enum class SlotStatus { active, disconnected, expired };

struct GameSlotRecord {
    std::string  session_id;   // UUID
    int64_t      player_id;
    int          slot_index;
    SlotStatus   status;
    int          pending_payout = 0;
    std::optional<std::chrono::system_clock::time_point> disconnected_at;
};

class GameSlotsRepo {
public:
    explicit GameSlotsRepo(DbPool& pool);
    ~GameSlotsRepo();

    // Upserts a row with status='active'. On conflict (session_id, slot_index)
    // resets player_id, clears disconnected_at, and sets status='active'.
    void upsert_active(const std::string& session_id,
                       int64_t player_id,
                       int slot_index);

    void mark_disconnected(const std::string& session_id, int slot_index);
    void mark_expired     (const std::string& session_id, int slot_index);
    void set_payout       (const std::string& session_id, int slot_index, int payout);
    // Clears disconnected_at and resets status to 'active'.
    void mark_reattached  (const std::string& session_id, int slot_index);

    // Sync read — used only at game end for payout reconciliation.
    std::vector<GameSlotRecord> get_by_session(const std::string& session_id);

private:
    DbPool& pool_;
    std::mutex futures_mu_;
    std::vector<std::future<void>> futures_;

    template<typename F>
    void async_write(F&& fn);

    static std::chrono::system_clock::time_point parse_ts(const std::string& s);
    static SlotStatus parse_status(const std::string& s);
};

} // namespace anjeer::server
