#pragma once

#include "server/bot_adapter.h"
#include "server/bot_scheduler.h"
#include "server/bot_spawn_context.h"
#include "server/config.h"
#include "engine/bots/bot_types.h"

#include <readerwriterqueue.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace anjeer::server {

struct BotSlotInfo {
    std::string bot_uuid;
    std::string difficulty;  // "easy" | "medium" | "hard"
    std::string username;    // display name shown in lobby + game roster
    int         slot;        // -1 until attach_to_session
};

// BotManager is accessed exclusively from the uWS event-loop thread.
// No internal locking is needed; the uWS guarantee of single-threaded access
// to all event callbacks and timers extends to every public method here.
class BotManager {
public:
    BotManager(
        BotScheduler&                    scheduler,
        const ServerConfig::BotsConfig&  bots_cfg
    );

    // ── Lobby phase (uWS thread) ──────────────────────────────────────────

    // Add a bot to a lobby. open_slots = max_players − current total players.
    // Returns {true, bot_uuid} on success, {false, error_code} on failure.
    std::pair<bool, std::string> add_bot(
        const std::string&                lobby_id,
        anjeer::engine::BotDifficulty     difficulty,
        int                               open_slots
    );

    // Remove a bot from a lobby before the game starts.
    // Returns false if bot_uuid is not found in the lobby.
    bool remove_bot(const std::string& lobby_id, const std::string& bot_uuid);

    // ── Game start (uWS thread) ───────────────────────────────────────────

    // Create BotAdapters and register tick callbacks with the scheduler.
    // slot_map: bot_uuid → assigned player slot index.
    // WsServer is responsible for enqueuing NetConnect for each bot slot
    // into the session's inbound queue after this call.
    void attach_to_session(
        const std::string&                              lobby_id,
        const std::unordered_map<std::string, int>&     slot_map,
        int32_t                                         points_per_card,
        int32_t                                         buy_in,
        int                                             round_duration_s,
        bool                                            is_ui_mode = false
    );

    // ── Drain loop (uWS timer, 16 ms) ────────────────────────────────────

    // Fan-out a game event to each adapter whose slot matches or is a broadcast.
    // target_slot == -1 means broadcast to all bots.
    void dispatch_to_bots(
        const std::string& lobby_id,
        std::string_view   json_payload,
        int                target_slot
    );

    // Drain all adapter action_queues into the session's inbound queue.
    void drain_bot_actions(
        const std::string&                         lobby_id,
        moodycamel::ReaderWriterQueue<NetEvent>&   session_inbound
    );

    // ── Mid-round replacement (uWS drain loop) ───────────────────────────────

    // Spawn a replacement bot for a slot vacated mid-round.
    // Synthesizes a round_start event from the departing player's hand/balance/time.
    // Bot persists for the rest of the game.
    // Enqueues NetConnect into session_inbound to re-activate the slot in GameSession.
    void spawn_replacement(
        const std::string&                      lobby_id,
        const BotSpawnContext&                  ctx,
        moodycamel::ReaderWriterQueue<NetEvent>& session_inbound,
        bool                                    is_ui_mode = false
    );

    // Remove the bot occupying a specific slot (seam for future real-player takeover).
    // Returns false if no bot is found at that slot in this lobby.
    bool remove_bot_for_slot(const std::string& lobby_id, int slot);

    // ── Game end (uWS thread) ─────────────────────────────────────────────

    void teardown_session(const std::string& lobby_id);

    // ── Queries ───────────────────────────────────────────────────────────

    bool                    has_bots(const std::string& lobby_id) const;
    std::vector<BotSlotInfo> get_bots(const std::string& lobby_id) const;

private:
    struct BotEntry {
        std::string                      bot_uuid;
        anjeer::engine::BotDifficulty    difficulty;
        BotHandle                        handle{0};
        std::unique_ptr<BotAdapter>      adapter;
        int                              slot{-1};
        bool                             is_replacement = false; // spawned mid-round
    };

    BotScheduler&                   scheduler_;
    ServerConfig::BotsConfig        bots_cfg_;
    std::mt19937_64                 rng_;

    // lobby_id → bots for that lobby
    std::unordered_map<std::string, std::vector<BotEntry>> sessions_;

    anjeer::engine::BotConfig             make_engine_config(anjeer::engine::BotDifficulty d) const;
    const ServerConfig::BotsConfig::PerDifficultyParams& select_difficulty_params(anjeer::engine::BotDifficulty d) const;
    static std::string                    difficulty_str(anjeer::engine::BotDifficulty d) noexcept;
    static std::string        bot_username(anjeer::engine::BotDifficulty d, int index) noexcept;
    std::string               generate_bot_id();
};

} // namespace anjeer::server
