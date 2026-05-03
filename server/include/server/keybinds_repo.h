#pragma once

#include <string>
#include <vector>

#include "server/db.h"

namespace anjeer::server {

struct KeyBind {
    std::string action;
    std::string key_combo;
};

class KeybindsRepo {
public:
    std::vector<KeyBind> get(DbTxn& txn, int64_t player_id);

    // Full replace: deletes all rows for player_id, then inserts binds.
    void set(DbTxn& txn, int64_t player_id, const std::vector<KeyBind>& binds);
};

} // namespace anjeer::server
