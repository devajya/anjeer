#pragma once

#include <optional>
#include <string>
#include <vector>

#include "server/db.h"

namespace anjeer::server {

// AGENT-CTX: LobbyStatus mirrors the DB CHECK constraint on lobbies.status.
// status_str() / parse_status() are the single source of truth for the
// string ↔ enum mapping — never inline these strings elsewhere in the server.
enum class LobbyStatus { Waiting, Starting, InGame, Finished, Closed };

// AGENT-CTX: LobbyMode mirrors the DB CHECK constraint on lobbies.mode.
// lobby_mode_string() / parse_mode() are the single source of truth for the
// string ↔ enum mapping — never inline "ui" / "api" elsewhere in the server.
enum class LobbyMode { UI, API };

std::string lobby_status_string(LobbyStatus s);
std::string lobby_mode_string  (LobbyMode m);
LobbyMode   parse_lobby_mode   (const std::string& s);

struct Lobby {
    std::string id;           // UUID as string
    std::string code;         // CHAR(6) A-Z0-9
    int64_t     creator_id;
    LobbyStatus status;
    int         min_players;
    int         max_players;
    std::string created_at;
    LobbyMode   mode = LobbyMode::UI;
};

// AGENT-CTX: LobbyView is a read-only projection for list responses only.
// player_count is computed via COUNT(*) in list_waiting — never stored.
struct LobbyView {
    Lobby lobby;
    int   player_count;
};

struct LobbyPlayer {
    std::string lobby_id;
    int64_t     player_id;
    std::string username;
    std::string joined_at;
};

class LobbyRepo {
public:
    // Creates lobby, generates unique 6-char code. Retries on UNIQUE collision.
    Lobby create(DbTxn& txn, int64_t creator_id,
                 int min_players, int max_players,
                 LobbyMode mode = LobbyMode::UI);

    std::optional<Lobby> find_by_id  (DbTxn& txn,
                                      const std::string& lobby_id);
    std::optional<Lobby> find_by_code(DbTxn& txn,
                                      const std::string& code);

    std::vector<LobbyView> list_waiting(DbTxn& txn,
                                         std::optional<LobbyMode> mode = std::nullopt);
    std::vector<LobbyView> list_active (DbTxn& txn,
                                         std::optional<LobbyMode> mode = std::nullopt);

    // AGENT-CTX: Re-joining is idempotent (returns existing joined_at) — the
    // "already joined → nullopt" guard was removed in Slice 7 so leave+rejoin
    // works. Nullopt is returned only when lobby is full or not in waiting state.
    std::optional<std::string> add_player(DbTxn& txn,
                                          const std::string& lobby_id,
                                          int64_t player_id);

    bool remove_player  (DbTxn& txn, const std::string& lobby_id, int64_t player_id);
    // AGENT-CTX: Atomically deletes lobby only when lobby_players count is 0.
    // Called after every departure so abandoned lobbies never accumulate.
    bool delete_if_empty(DbTxn& txn, const std::string& lobby_id);

    // Returns all players in a lobby (username joined from players table).
    std::vector<LobbyPlayer> list_players(DbTxn& txn,
                                          const std::string& lobby_id);

    // Returns current player count via COUNT(*).
    int player_count(DbTxn& txn, const std::string& lobby_id);

    // CAS-style status transition. Returns false if current status != from.
    bool transition_status(DbTxn& txn,
                           const std::string& lobby_id,
                           LobbyStatus from, LobbyStatus to);

private:
    static Lobby       row_to_lobby (const pqxx::row& row);
    static LobbyPlayer row_to_player(const pqxx::row& row);
    static std::string status_str   (LobbyStatus s);
    static LobbyStatus parse_status (const std::string& s);
    static std::string mode_str     (LobbyMode m);
    // Generates 6-char A-Z0-9 code via OpenSSL RAND_bytes.
    static std::string generate_code();
};

} // namespace anjeer::server
