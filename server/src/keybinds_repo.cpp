#include "server/keybinds_repo.h"

namespace anjeer::server {

std::vector<KeyBind> KeybindsRepo::get(DbTxn& txn, int64_t player_id) {
    auto rows = txn.exec_params(
        "SELECT action, key_combo FROM player_keybinds WHERE player_id = $1",
        player_id
    );
    std::vector<KeyBind> result;
    result.reserve(rows.size());
    for (const auto& row : rows) {
        result.push_back({
            row["action"].as<std::string>(),
            row["key_combo"].as<std::string>()
        });
    }
    return result;
}

void KeybindsRepo::set(DbTxn& txn, int64_t player_id, const std::vector<KeyBind>& binds) {
    txn.exec_params(
        "DELETE FROM player_keybinds WHERE player_id = $1",
        player_id
    );
    for (const auto& b : binds) {
        txn.exec_params(
            "INSERT INTO player_keybinds (player_id, action, key_combo) VALUES ($1, $2, $3)",
            player_id, b.action, b.key_combo
        );
    }
}

} // namespace anjeer::server
