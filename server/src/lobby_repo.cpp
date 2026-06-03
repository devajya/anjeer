#include "server/lobby_repo.h"

#include <stdexcept>

#include <openssl/rand.h>

namespace anjeer::server {

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

Lobby LobbyRepo::row_to_lobby(const pqxx::row& row) {
    Lobby l;
    l.id                   = row["id"].as<std::string>();
    l.code                 = row["code"].as<std::string>();
    l.creator_id           = row["creator_id"].as<int64_t>();
    l.status               = parse_status(row["status"].as<std::string>());
    l.min_players          = row["min_players"].as<int>();
    l.max_players          = row["max_players"].as<int>();
    l.created_at           = row["created_at"].as<std::string>();
    l.mode                 = parse_lobby_mode(row["mode"].as<std::string>());
    l.spawn_bots_on_leave  = row["spawn_bots_on_leave"].as<bool>();
    l.bot_spawn_difficulty = row["bot_spawn_difficulty"].as<std::string>();
    l.bot_count            = row["bot_count"].as<int>();
    l.wipe_on_trade        = row["wipe_on_trade"].as<bool>();
    return l;
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

std::string lobby_mode_string(LobbyMode m) {
    switch (m) {
        case LobbyMode::UI:  return "ui";
        case LobbyMode::API: return "api";
    }
    throw std::runtime_error("lobby_mode_string: unknown LobbyMode value");
}

LobbyMode parse_lobby_mode(const std::string& s) {
    if (s == "ui")  return LobbyMode::UI;
    if (s == "api") return LobbyMode::API;
    throw std::runtime_error("parse_lobby_mode: unknown mode string: " + s);
}

std::string LobbyRepo::status_str(LobbyStatus s) { return lobby_status_string(s); }
std::string LobbyRepo::mode_str  (LobbyMode m)   { return lobby_mode_string(m);   }

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
                         int min_players, int max_players, LobbyMode mode,
                         bool spawn_bots_on_leave, std::string bot_spawn_difficulty,
                         bool wipe_on_trade) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        const auto code = generate_code();
        const auto exists = txn.exec_params(
            "SELECT 1 FROM lobbies WHERE code = $1", code
        );
        if (!exists.empty()) continue;

        const auto r = txn.exec_params(
            "INSERT INTO lobbies (code, creator_id, min_players, max_players, mode, "
            "                     spawn_bots_on_leave, bot_spawn_difficulty, wipe_on_trade) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8) "
            "RETURNING id, code, creator_id, status, min_players, max_players, created_at, mode, "
            "          spawn_bots_on_leave, bot_spawn_difficulty, bot_count, wipe_on_trade",
            code, creator_id, min_players, max_players, mode_str(mode),
            spawn_bots_on_leave, bot_spawn_difficulty, wipe_on_trade
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
        "SELECT id, code, creator_id, status, min_players, max_players, created_at, mode, "
        "       spawn_bots_on_leave, bot_spawn_difficulty, bot_count, wipe_on_trade "
        "FROM lobbies WHERE id = $1",
        lobby_id
    );
    if (r.empty()) return std::nullopt;
    return row_to_lobby(r[0]);
}

std::optional<Lobby> LobbyRepo::find_by_code(pqxx::transaction_base& txn,
                                               const std::string& code) {
    const auto r = txn.exec_params(
        "SELECT id, code, creator_id, status, min_players, max_players, created_at, mode, "
        "       spawn_bots_on_leave, bot_spawn_difficulty, bot_count, wipe_on_trade "
        "FROM lobbies WHERE code = $1",
        code
    );
    if (r.empty()) return std::nullopt;
    return row_to_lobby(r[0]);
}

std::vector<LobbyView> LobbyRepo::list_waiting(pqxx::transaction_base& txn,
                                                  std::optional<LobbyMode> mode) {
    std::string sql =
        "SELECT l.id, l.code, l.creator_id, l.status, l.min_players, l.max_players, "
        "       l.created_at, l.mode, l.spawn_bots_on_leave, l.bot_spawn_difficulty, l.bot_count, l.wipe_on_trade, "
        "       COUNT(lp.player_id) AS player_count "
        "FROM lobbies l "
        "LEFT JOIN lobby_players lp ON lp.lobby_id = l.id "
        "WHERE l.status = 'waiting'";
    if (mode) sql += " AND l.mode = '" + mode_str(*mode) + "'";
    sql += " GROUP BY l.id ORDER BY l.created_at DESC";

    const auto r = txn.exec(sql);
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

std::vector<LobbyView> LobbyRepo::list_active(pqxx::transaction_base& txn,
                                                std::optional<LobbyMode> mode) {
    std::string sql =
        "SELECT l.id, l.code, l.creator_id, l.status, l.min_players, l.max_players, "
        "       l.created_at, l.mode, l.spawn_bots_on_leave, l.bot_spawn_difficulty, l.bot_count, l.wipe_on_trade, "
        "       COUNT(lp.player_id) AS player_count "
        "FROM lobbies l "
        "LEFT JOIN lobby_players lp ON lp.lobby_id = l.id "
        "WHERE l.status = 'in_game'";
    if (mode) sql += " AND l.mode = '" + mode_str(*mode) + "'";
    sql += " GROUP BY l.id ORDER BY l.created_at DESC";

    const auto r = txn.exec(sql);
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

void LobbyRepo::update_bot_settings(pqxx::transaction_base& txn,
                                     const std::string& lobby_id,
                                     bool spawn_bots_on_leave,
                                     const std::string& bot_spawn_difficulty) {
    txn.exec_params(
        "UPDATE lobbies SET spawn_bots_on_leave = $2, bot_spawn_difficulty = $3 WHERE id = $1",
        lobby_id, spawn_bots_on_leave, bot_spawn_difficulty
    );
}

void LobbyRepo::update_wipe_on_trade(pqxx::transaction_base& txn,
                                      const std::string& lobby_id,
                                      bool wipe_on_trade) {
    txn.exec_params(
        "UPDATE lobbies SET wipe_on_trade = $2 WHERE id = $1",
        lobby_id, wipe_on_trade
    );
}

void LobbyRepo::adjust_bot_count(pqxx::transaction_base& txn,
                                  const std::string& lobby_id,
                                  int delta) {
    txn.exec_params(
        "UPDATE lobbies SET bot_count = GREATEST(0, bot_count + $2) WHERE id = $1",
        lobby_id, delta
    );
}

} // namespace anjeer::server
