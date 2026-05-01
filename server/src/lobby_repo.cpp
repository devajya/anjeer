#include "server/lobby_repo.h"

#include <stdexcept>

#include <openssl/rand.h>

namespace anjeer::server {

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

Lobby LobbyRepo::row_to_lobby(const pqxx::row& row) {
    return Lobby{
        row["id"].as<std::string>(),
        row["code"].as<std::string>(),
        row["creator_id"].as<int64_t>(),
        parse_status(row["status"].as<std::string>()),
        row["min_players"].as<int>(),
        row["max_players"].as<int>(),
        row["created_at"].as<std::string>()
    };
}

LobbyPlayer LobbyRepo::row_to_player(const pqxx::row& row) {
    return LobbyPlayer{
        row["lobby_id"].as<std::string>(),
        row["player_id"].as<int64_t>(),
        row["username"].as<std::string>(),
        row["joined_at"].as<std::string>()
    };
}

std::string lobby_status_string(LobbyStatus s) {
    switch (s) {
        case LobbyStatus::Waiting:  return "waiting";
        case LobbyStatus::Starting: return "starting";
        case LobbyStatus::InGame:   return "in_game";
        case LobbyStatus::Finished: return "finished";
        case LobbyStatus::Closed:   return "closed";
    }
    throw std::runtime_error("lobby_status_string: unknown LobbyStatus value");
}

std::string LobbyRepo::status_str(LobbyStatus s) { return lobby_status_string(s); }

LobbyStatus LobbyRepo::parse_status(const std::string& s) {
    if (s == "waiting")  return LobbyStatus::Waiting;
    if (s == "starting") return LobbyStatus::Starting;
    if (s == "in_game")  return LobbyStatus::InGame;
    if (s == "finished") return LobbyStatus::Finished;
    if (s == "closed")   return LobbyStatus::Closed;
    throw std::runtime_error("LobbyRepo::parse_status: unknown status string: " + s);
}

// Modulo bias (256/36 ≈ 7.1) is negligible for 6-char join codes at Slice 6 scale.
std::string LobbyRepo::generate_code() {
    static constexpr char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    static constexpr int  len       = 6;
    unsigned char         buf[len];
    RAND_bytes(buf, len);
    std::string code(len, '\0');
    for (int i = 0; i < len; ++i)
        code[i] = charset[buf[i] % 36];
    return code;
}

// ---------------------------------------------------------------------------
// Public methods
// ---------------------------------------------------------------------------

// SELECT-before-INSERT avoids pqxx::subtransaction, which requires dbtransaction&
// (incompatible with our DbTxn = transaction_base signature).
Lobby LobbyRepo::create(pqxx::transaction_base& txn, int64_t creator_id,
                         int min_players, int max_players) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        const auto code = generate_code();
        const auto exists = txn.exec_params(
            "SELECT 1 FROM lobbies WHERE code = $1", code
        );
        if (!exists.empty()) continue;

        const auto r = txn.exec_params(
            "INSERT INTO lobbies (code, creator_id, min_players, max_players) "
            "VALUES ($1, $2, $3, $4) "
            "RETURNING id, code, creator_id, status, min_players, max_players, created_at",
            code, creator_id, min_players, max_players
        );
        if (r.empty())
            throw std::runtime_error("LobbyRepo::create: INSERT RETURNING returned no rows");

        auto lobby = row_to_lobby(r[0]);

        // AGENT-CTX: Creator row inserted at lobby creation so player_count()
        // is never 0 after create(). Slice 7 resilience requirement: the old
        // code had a window where the lobby existed with no player rows.
        txn.exec_params(
            "INSERT INTO lobby_players (lobby_id, player_id) VALUES ($1, $2)",
            lobby.id, creator_id
        );

        return lobby;
    }
    throw std::runtime_error("LobbyRepo::create: failed to generate unique code after 10 attempts");
}

std::optional<Lobby> LobbyRepo::find_by_id(pqxx::transaction_base& txn,
                                             const std::string& lobby_id) {
    const auto r = txn.exec_params(
        "SELECT id, code, creator_id, status, min_players, max_players, created_at "
        "FROM lobbies WHERE id = $1",
        lobby_id
    );
    if (r.empty()) return std::nullopt;
    return row_to_lobby(r[0]);
}

std::optional<Lobby> LobbyRepo::find_by_code(pqxx::transaction_base& txn,
                                               const std::string& code) {
    const auto r = txn.exec_params(
        "SELECT id, code, creator_id, status, min_players, max_players, created_at "
        "FROM lobbies WHERE code = $1",
        code
    );
    if (r.empty()) return std::nullopt;
    return row_to_lobby(r[0]);
}

std::vector<LobbyView> LobbyRepo::list_waiting(pqxx::transaction_base& txn) {
    const auto r = txn.exec(
        "SELECT l.id, l.code, l.creator_id, l.status, l.min_players, l.max_players, "
        "       l.created_at, COUNT(lp.player_id) AS player_count "
        "FROM lobbies l "
        "LEFT JOIN lobby_players lp ON lp.lobby_id = l.id "
        "WHERE l.status = 'waiting' "
        "GROUP BY l.id "
        "ORDER BY l.created_at DESC"
    );
    std::vector<LobbyView> views;
    views.reserve(r.size());
    for (const auto& row : r) {
        LobbyView v;
        v.lobby        = row_to_lobby(row);
        v.player_count = row["player_count"].as<int>();
        views.push_back(std::move(v));
    }
    return views;
}

std::vector<LobbyView> LobbyRepo::list_active(pqxx::transaction_base& txn) {
    const auto r = txn.exec(
        "SELECT l.id, l.code, l.creator_id, l.status, l.min_players, l.max_players, "
        "       l.created_at, COUNT(lp.player_id) AS player_count "
        "FROM lobbies l "
        "LEFT JOIN lobby_players lp ON lp.lobby_id = l.id "
        "WHERE l.status = 'in_game' "
        "GROUP BY l.id "
        "ORDER BY l.created_at DESC"
    );
    std::vector<LobbyView> views;
    views.reserve(r.size());
    for (const auto& row : r) {
        LobbyView v;
        v.lobby        = row_to_lobby(row);
        v.player_count = row["player_count"].as<int>();
        views.push_back(std::move(v));
    }
    return views;
}

std::optional<std::string> LobbyRepo::add_player(pqxx::transaction_base& txn,
                                                    const std::string& lobby_id,
                                                    int64_t player_id) {
    // AGENT-CTX: BOOL_OR detects if player already has a row in one pass.
    // already_joined=true → idempotent return of existing joined_at (Slice 7:
    // leave + rejoin must work; the old explicit duplicate guard is removed).
    const auto r = txn.exec_params(
        "SELECT l.status, l.max_players, COUNT(lp.player_id) AS cnt, "
        "       BOOL_OR(lp.player_id = $2) AS already_joined "
        "FROM lobbies l "
        "LEFT JOIN lobby_players lp ON lp.lobby_id = l.id "
        "WHERE l.id = $1 "
        "GROUP BY l.id, l.status, l.max_players",
        lobby_id, player_id
    );

    if (r.empty()) return std::nullopt;
    if (parse_status(r[0]["status"].as<std::string>()) != LobbyStatus::Waiting)
        return std::nullopt;

    const bool already_joined = r[0]["already_joined"].as<bool>(false);
    if (already_joined) {
        const auto existing = txn.exec_params(
            "SELECT joined_at FROM lobby_players WHERE lobby_id = $1 AND player_id = $2",
            lobby_id, player_id
        );
        if (existing.empty()) return std::nullopt;
        return existing[0][0].as<std::string>();
    }

    if (r[0]["cnt"].as<int>() >= r[0]["max_players"].as<int>())
        return std::nullopt;

    const auto ins = txn.exec_params(
        "INSERT INTO lobby_players (lobby_id, player_id) VALUES ($1, $2) "
        "RETURNING joined_at",
        lobby_id, player_id
    );
    return ins[0]["joined_at"].as<std::string>();
}

bool LobbyRepo::remove_player(pqxx::transaction_base& txn,
                                const std::string& lobby_id, int64_t player_id) {
    const auto r = txn.exec_params(
        "DELETE FROM lobby_players WHERE lobby_id = $1 AND player_id = $2",
        lobby_id, player_id
    );
    return r.affected_rows() > 0;
}

bool LobbyRepo::delete_if_empty(pqxx::transaction_base& txn,
                                  const std::string& lobby_id) {
    // AGENT-CTX: Subquery makes the delete atomic — no TOCTOU race if a player
    // joins between count-check and delete. ON DELETE CASCADE on lobby_players
    // is a safety net but should never fire here since count is verified 0.
    const auto r = txn.exec_params(
        "DELETE FROM lobbies WHERE id = $1 "
        "AND (SELECT COUNT(*) FROM lobby_players WHERE lobby_id = $1) = 0",
        lobby_id
    );
    return r.affected_rows() > 0;
}

std::vector<LobbyPlayer> LobbyRepo::list_players(pqxx::transaction_base& txn,
                                                   const std::string& lobby_id) {
    const auto r = txn.exec_params(
        "SELECT lp.lobby_id, lp.player_id, p.username, lp.joined_at "
        "FROM lobby_players lp "
        "JOIN players p ON lp.player_id = p.id "
        "WHERE lp.lobby_id = $1 "
        "ORDER BY lp.joined_at ASC",
        lobby_id
    );
    std::vector<LobbyPlayer> players;
    players.reserve(r.size());
    for (const auto& row : r)
        players.push_back(row_to_player(row));
    return players;
}

int LobbyRepo::player_count(pqxx::transaction_base& txn, const std::string& lobby_id) {
    const auto r = txn.exec_params(
        "SELECT COUNT(*) FROM lobby_players WHERE lobby_id = $1",
        lobby_id
    );
    return r[0][0].as<int>();
}

// CAS via WHERE status=$from; false means a concurrent transition already occurred.
bool LobbyRepo::transition_status(pqxx::transaction_base& txn,
                                    const std::string& lobby_id,
                                    LobbyStatus from, LobbyStatus to) {
    const auto r = txn.exec_params(
        "UPDATE lobbies SET status = $3 WHERE id = $1 AND status = $2",
        lobby_id, status_str(from), status_str(to)
    );
    return r.affected_rows() > 0;
}

} // namespace anjeer::server
