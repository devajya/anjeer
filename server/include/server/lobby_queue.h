#pragma once

// AGENT-CTX: LobbyQueue holds players waiting to enter an already-active
// game session. It is owned by WsServer per lobby (added in T10) and called
// exclusively from the uWS event-loop thread — no locks needed.
//
// Positions are 1-based (position 1 = next to be admitted). enqueue()
// returns -1 on two error conditions: queue full and double-enqueue; callers
// distinguish them by checking has() beforehand if needed.
//
// broadcast_positions() sends queue_position_update to every waiting entry.
// It uses uWS::Loop::defer() so the send always executes on the event-loop
// thread even if called from a timer or background path.

#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

#include "server/ws_types.h"

namespace anjeer::server {

struct QueueEntry {
    int64_t      player_id;
    std::string  username;
    WsHandle     ws;
    std::chrono::system_clock::time_point enqueued_at;
};

class LobbyQueue {
public:
    explicit LobbyQueue(int max_size);

    // Returns 1-based position on success, -1 if full or already enqueued.
    int  enqueue(int64_t player_id, const std::string& username, WsHandle ws);
    void dequeue(int64_t player_id);

    // Removes and returns up to count entries from the front.
    std::vector<QueueEntry> drain(int count);

    // Returns 1-based position, or 0 if the player is not in the queue.
    int  position_of(int64_t player_id) const;

    int  size() const;
    bool has(int64_t player_id) const;

    // Sends queue_position_update to every waiting entry via loop->defer().
    void broadcast_positions(uWS::Loop* loop) const;

private:
    int max_size_;
    std::deque<QueueEntry>           entries_;
    std::unordered_set<int64_t>      id_set_;  // O(1) membership check
};

} // namespace anjeer::server
