#include "server/lobby_queue.h"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace anjeer::server {

LobbyQueue::LobbyQueue(int max_size) : max_size_(max_size) {}

int LobbyQueue::enqueue(int64_t player_id,
                        const std::string& username,
                        WsHandle ws) {
    if (id_set_.count(player_id) || static_cast<int>(entries_.size()) >= max_size_)
        return -1;

    entries_.push_back({ player_id, username, ws,
                         std::chrono::system_clock::now() });
    id_set_.insert(player_id);
    return static_cast<int>(entries_.size());  // 1-based position
}

void LobbyQueue::dequeue(int64_t player_id) {
    auto it = std::find_if(entries_.begin(), entries_.end(),
        [&](const QueueEntry& e) { return e.player_id == player_id; });
    if (it == entries_.end()) return;
    entries_.erase(it);
    id_set_.erase(player_id);
}

std::vector<QueueEntry> LobbyQueue::drain(int count) {
    const int n = std::min(count, static_cast<int>(entries_.size()));
    std::vector<QueueEntry> out(
        std::make_move_iterator(entries_.begin()),
        std::make_move_iterator(entries_.begin() + n)
    );
    entries_.erase(entries_.begin(), entries_.begin() + n);
    for (const auto& e : out) id_set_.erase(e.player_id);
    return out;
}

int LobbyQueue::position_of(int64_t player_id) const {
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i)
        if (entries_[i].player_id == player_id) return i + 1;
    return 0;
}

int LobbyQueue::size() const {
    return static_cast<int>(entries_.size());
}

bool LobbyQueue::has(int64_t player_id) const {
    return id_set_.count(player_id) > 0;
}

void LobbyQueue::broadcast_positions(uWS::Loop* loop) const {
    // Snapshot entries so the lambda captures values, not iterators.
    const int total = static_cast<int>(entries_.size());

    for (int i = 0; i < total; ++i) {
        const QueueEntry& entry = entries_[i];
        if (!entry.ws) continue;

        const int pos = i + 1;

        // Build a small window of players_around (up to 2 before and 2 after,
        // always including self). Window size is arbitrary — wide enough for
        // visual context without sending the full queue to every waiter.
        nlohmann::json around = nlohmann::json::array();
        const int window_start = std::max(0, i - 2);
        const int window_end   = std::min(total - 1, i + 2);
        for (int j = window_start; j <= window_end; ++j) {
            around.push_back({
                {"position", j + 1},
                {"username", entries_[j].username},
                {"is_self",  j == i}
            });
        }

        nlohmann::json msg = {
            {"type",           "queue_position_update"},
            {"position",       pos},
            {"queue_size",     total},
            {"players_around", around}
        };
        const std::string payload = msg.dump();

        WsHandle ws = entry.ws;
        loop->defer([ws, payload]() {
            if (ws) ws->send(payload, uWS::OpCode::TEXT);
        });
    }
}

} // namespace anjeer::server
