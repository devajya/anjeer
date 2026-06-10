#include "server/bot_manager.h"

#include "engine/bots/bot_agent.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <random>

namespace anjeer::server {

BotManager::BotManager(BotScheduler& scheduler, const ServerConfig::BotsConfig& bots_cfg)
    : scheduler_(scheduler)
    , bots_cfg_(bots_cfg)
    , rng_(std::random_device{}())
{}

// ── Lobby phase ───────────────────────────────────────────────────────────────

BotAddResult BotManager::add_bot(
        const std::string&             lobby_id,
        anjeer::engine::BotDifficulty  difficulty,
        int                            open_slots) {
    if (open_slots <= 0)
        return {false, "BOT_LIMIT_REACHED", ""};

    auto& bots = sessions_[lobby_id];

    // Check a game hasn't already started for this lobby.
    for (const auto& e : bots) {
        if (e.adapter) return {false, "LOBBY_ALREADY_STARTED", ""};
    }

    BotEntry entry;
    entry.bot_uuid   = generate_bot_id();
    entry.difficulty = difficulty;
    bots.push_back(std::move(entry));

    const int index    = static_cast<int>(bots.size()) - 1;
    const std::string username = bot_username(difficulty, index);
    return {true, bots.back().bot_uuid, username};
}

BotRemoveResult BotManager::remove_bot(const std::string& lobby_id, const std::string& bot_uuid) {
    auto sit = sessions_.find(lobby_id);
    if (sit == sessions_.end()) return {false, ""};

    auto& bots = sit->second;
    auto it = std::find_if(bots.begin(), bots.end(),
        [&](const BotEntry& e){ return e.bot_uuid == bot_uuid; });
    if (it == bots.end()) return {false, ""};

    // Only allow removal before game starts.
    if (it->adapter) return {false, ""};

    const int index = static_cast<int>(it - bots.begin());
    const std::string username = bot_username(it->difficulty, index);
    bots.erase(it);
    if (bots.empty()) sessions_.erase(sit);
    return {true, username};
}

// ── Game start ────────────────────────────────────────────────────────────────

void BotManager::attach_to_session(
        const std::string&                          lobby_id,
        const std::unordered_map<std::string, int>& slot_map,
        int32_t                                     points_per_card,
        int32_t                                     buy_in,
        int                                         round_duration_s,
        bool                                        is_ui_mode) {
    auto sit = sessions_.find(lobby_id);
    if (sit == sessions_.end()) return;

    // Create per-lobby log directory once.
    std::string log_dir = "logs/games/" + lobby_id + "/bots";
    std::filesystem::create_directories(log_dir);

    for (auto& entry : sit->second) {
        auto slot_it = slot_map.find(entry.bot_uuid);
        if (slot_it == slot_map.end()) continue;
        entry.slot = slot_it->second;

        const auto& dp    = select_difficulty_params(entry.difficulty);
        auto engine_cfg   = make_engine_config(entry.difficulty);
        uint32_t seed     = static_cast<uint32_t>(rng_());
        auto strategy     = anjeer::engine::make_bot(entry.difficulty, engine_cfg, seed);

        std::string log_path = log_dir + "/bot-" + difficulty_str(entry.difficulty)
                             + "-slot" + std::to_string(entry.slot) + ".log";

        int think_min = is_ui_mode ? dp.thinking_min_ms : 0;
        int think_max = is_ui_mode ? dp.thinking_max_ms : 0;

        entry.adapter = std::make_unique<BotAdapter>(
            std::move(strategy),
            BotSpawnContext{entry.slot, {}, 0, 0.0f, difficulty_str(entry.difficulty),
                            points_per_card, buy_in, round_duration_s},
            bots_cfg_.sim_network_delay_ms,
            dp.tick_interval_ms,
            dp.tick_jitter_ms,
            think_min,
            think_max,
            dp.max_concurrent_orders,
            entry.bot_uuid,
            log_path
        );

        auto* adapter_ptr = entry.adapter.get();
        entry.handle = scheduler_.register_bot(
            [adapter_ptr]{ adapter_ptr->tick(); },
            bots_cfg_.scheduler_tick_ms
        );
    }
}

// ── Drain loop ────────────────────────────────────────────────────────────────

void BotManager::dispatch_to_bots(const std::string& lobby_id,
                                   std::string_view   json_payload,
                                   int                target_slot) {
    auto sit = sessions_.find(lobby_id);
    if (sit == sessions_.end()) return;

    for (auto& entry : sit->second) {
        if (!entry.adapter || !entry.adapter->is_alive()) continue;
        if (target_slot != -1 && entry.slot != target_slot) continue;
        entry.adapter->on_game_event(json_payload, target_slot != -1);
    }
}

void BotManager::drain_bot_actions(const std::string& lobby_id,
                                    moodycamel::ReaderWriterQueue<NetEvent>& session_inbound) {
    auto sit = sessions_.find(lobby_id);
    if (sit == sessions_.end()) return;

    for (auto& entry : sit->second) {
        if (entry.adapter) entry.adapter->drain_to_session(session_inbound);
    }
}

// ── Game end ──────────────────────────────────────────────────────────────────

void BotManager::teardown_session(const std::string& lobby_id) {
    auto sit = sessions_.find(lobby_id);
    if (sit == sessions_.end()) return;

    for (auto& entry : sit->second) {
        if (entry.handle) {
            // teardown() first so tick() sees alive_=false before deregister waits.
            if (entry.adapter) entry.adapter->teardown();
            scheduler_.deregister_bot(entry.handle);
            entry.handle = 0;
        }
    }

    sessions_.erase(sit);
}

// ── Queries ───────────────────────────────────────────────────────────────────

bool BotManager::has_bots(const std::string& lobby_id) const {
    auto it = sessions_.find(lobby_id);
    return it != sessions_.end() && !it->second.empty();
}

std::vector<BotSlotInfo> BotManager::get_bots(const std::string& lobby_id) const {
    auto it = sessions_.find(lobby_id);
    if (it == sessions_.end()) return {};

    std::vector<BotSlotInfo> result;
    result.reserve(it->second.size());
    int index = 0;
    for (const auto& e : it->second) {
        result.push_back({
            e.bot_uuid,
            difficulty_str(e.difficulty),
            bot_username(e.difficulty, index++),
            e.slot
        });
    }
    return result;
}

// ── Private helpers ───────────────────────────────────────────────────────────

// ── Mid-round replacement ─────────────────────────────────────────────────────

static std::string format_iso_from_now(float offset_s) {
    const auto tp = std::chrono::system_clock::now()
        + std::chrono::duration_cast<std::chrono::system_clock::duration>(
              std::chrono::duration<float>(offset_s));
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm gmt{};
    gmtime_r(&t, &gmt);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &gmt);
    return buf;
}

void BotManager::spawn_replacement(
        const std::string&                       lobby_id,
        const BotSpawnContext&                   ctx,
        moodycamel::ReaderWriterQueue<NetEvent>& session_inbound,
        bool                                     is_ui_mode) {
    const int          slot           = ctx.slot;
    const std::string& difficulty_str = ctx.difficulty_str;
    // Resolve difficulty — "random" picks uniformly from Easy/Medium/Hard.
    anjeer::engine::BotDifficulty diff = anjeer::engine::BotDifficulty::Easy;
    if (difficulty_str == "medium") {
        diff = anjeer::engine::BotDifficulty::Medium;
    } else if (difficulty_str == "hard") {
        diff = anjeer::engine::BotDifficulty::Hard;
    } else if (difficulty_str == "random") {
        std::uniform_int_distribution<int> pick(0, 2);
        switch (pick(rng_)) {
            case 1: diff = anjeer::engine::BotDifficulty::Medium; break;
            case 2: diff = anjeer::engine::BotDifficulty::Hard;   break;
            default: break;
        }
    }

    auto& bots = sessions_[lobby_id];

    BotEntry entry;
    entry.bot_uuid       = generate_bot_id();
    entry.difficulty     = diff;
    entry.slot           = slot;
    entry.is_replacement = true;

    const auto& dp        = select_difficulty_params(diff);
    auto engine_cfg       = make_engine_config(diff);
    const uint32_t seed   = static_cast<uint32_t>(rng_());
    auto strategy = anjeer::engine::make_bot(diff, engine_cfg, seed);

    std::string repl_log_dir  = "logs/games/" + lobby_id + "/bots";
    std::filesystem::create_directories(repl_log_dir);
    std::string repl_log_path = repl_log_dir + "/bot-" + BotManager::difficulty_str(diff)
                              + "-slot" + std::to_string(slot) + "-replacement.log";

    int think_min = is_ui_mode ? dp.thinking_min_ms : 0;
    int think_max = is_ui_mode ? dp.thinking_max_ms : 0;

    entry.adapter = std::make_unique<BotAdapter>(
        std::move(strategy),
        ctx,
        bots_cfg_.sim_network_delay_ms,
        dp.tick_interval_ms,
        dp.tick_jitter_ms,
        think_min,
        think_max,
        dp.max_concurrent_orders,
        entry.bot_uuid,
        repl_log_path
    );

    // Synthesize a round_start event so the bot knows its hand and time remaining.
    const int player_count = static_cast<int>(bots.size()) + 1;
    const nlohmann::json round_start_j{
        {"type",         "round_start"},
        {"player_slot",  slot},
        {"hand", {
            {"clubs",    ctx.hand[0]},
            {"diamonds", ctx.hand[1]},
            {"hearts",   ctx.hand[2]},
            {"spades",   ctx.hand[3]},
        }},
        {"round_end_at", format_iso_from_now(ctx.remaining_s)},
        {"balance",      ctx.balance},
        {"player_count", player_count},
    };
    entry.adapter->on_game_event(round_start_j.dump(), true);

    auto* adapter_ptr = entry.adapter.get();
    entry.handle = scheduler_.register_bot(
        [adapter_ptr]{ adapter_ptr->tick(); },
        bots_cfg_.scheduler_tick_ms
    );

    // Enqueue NetConnect so GameSession re-activates the slot for this bot.
    // Follow with NetSendFeedSnapshot so the session sends the right book snapshot type.
    const int64_t bot_pid    = -(static_cast<int64_t>(slot) + 1);
    const std::string bot_name = bot_username(diff, slot);
    session_inbound.enqueue(NetConnect{slot, bot_pid, bot_name});
    session_inbound.enqueue(NetSendFeedSnapshot{slot, ctx.feed});

    bots.push_back(std::move(entry));
}

bool BotManager::remove_bot_for_slot(const std::string& lobby_id, int slot) {
    auto sit = sessions_.find(lobby_id);
    if (sit == sessions_.end()) return false;

    auto& bots = sit->second;
    auto it = std::find_if(bots.begin(), bots.end(),
        [slot](const BotEntry& e) { return e.slot == slot; });
    if (it == bots.end()) return false;

    if (it->handle) {
        if (it->adapter) it->adapter->teardown();
        scheduler_.deregister_bot(it->handle);
    }
    bots.erase(it);
    if (bots.empty()) sessions_.erase(sit);
    return true;
}

const ServerConfig::BotsConfig::PerDifficultyParams&
BotManager::select_difficulty_params(anjeer::engine::BotDifficulty d) const {
    switch (d) {
        case anjeer::engine::BotDifficulty::Medium: return bots_cfg_.medium;
        case anjeer::engine::BotDifficulty::Hard:   return bots_cfg_.hard;
        default:                                    return bots_cfg_.easy;
    }
}

anjeer::engine::BotConfig BotManager::make_engine_config(anjeer::engine::BotDifficulty d) const {
    const auto& p = select_difficulty_params(d);
    anjeer::engine::BotConfig cfg;
    cfg.confidence_discount   = p.confidence_discount;
    cfg.taker_threshold       = p.taker_threshold;
    cfg.min_bid_ev            = p.min_bid_ev;
    cfg.max_ask_ev            = p.max_ask_ev;
    cfg.hand_size_cap         = p.hand_size_cap;
    cfg.offload_threshold     = p.offload_threshold;
    cfg.max_concurrent_orders = p.max_concurrent_orders;
    cfg.conviction_threshold  = p.conviction_threshold;
    cfg.max_resting_ms        = p.max_resting_ms;
    cfg.nudge_probability     = p.nudge_probability;
    cfg.nudge_patience_ms     = p.nudge_patience_ms;
    cfg.nudge_max_gap         = p.nudge_max_gap;
    cfg.endgame_threshold_s   = p.endgame_threshold_s;
    cfg.early_seed_threshold  = p.early_seed_threshold;
    cfg.quoting_kappa         = p.quoting_kappa;
    return cfg;
}

std::string BotManager::difficulty_str(anjeer::engine::BotDifficulty d) noexcept {
    switch (d) {
        case anjeer::engine::BotDifficulty::Easy:   return "easy";
        case anjeer::engine::BotDifficulty::Medium: return "medium";
        case anjeer::engine::BotDifficulty::Hard:   return "hard";
    }
    return "easy";
}

std::string BotManager::bot_username(anjeer::engine::BotDifficulty /*d*/, int index) noexcept {
    static constexpr const char* kNames[7] = {
        "SleepyLlama",   // Ollama
        "DockWhale",     // Docker
        "RustyCrab",     // Rust / Ferris
        "GopherBroke",   // Go gopher
        "TuxedoPenguin", // Linux / Tux
        "PythonBoa",     // Python
        "OctoProxy",     // GitHub Octocat
    };
    return kNames[index % 7];
}

std::string BotManager::generate_bot_id() {
    // AGENT-CTX: Switched from "bot_<16hex>" to RFC 4122 v4 UUID so that
    // bot_uuid values are proper UUID4s as required by the T7 spec. Existing
    // callers only compare for equality — no format assumption was baked in.
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t u1 = dist(rng_);
    uint64_t u2 = dist(rng_);
    // Set version 4 (bits 12-15 of u1's low 16 bits).
    u1 = (u1 & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    // Set variant bits 10xx (top 2 bits of u2).
    u2 = (u2 & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%012llx",
        (uint32_t)(u1 >> 32),
        (uint32_t)((u1 >> 16) & 0xFFFF),
        (uint32_t)(u1 & 0xFFFF),
        (uint32_t)(u2 >> 48),
        (unsigned long long)(u2 & 0x0000FFFFFFFFFFFFULL));
    return buf;
}

// ── New T7 methods ────────────────────────────────────────────────────────────

std::string BotManager::bot_uuid_for_slot(const std::string& lobby_id,
                                          int slot_index) const {
    auto sit = sessions_.find(lobby_id);
    if (sit == sessions_.end()) return "";
    for (const auto& entry : sit->second)
        if (entry.slot == slot_index) return entry.bot_uuid;
    return "";
}

int BotManager::get_displaceable_bot_slot(const std::string& lobby_id,
                                          const std::unordered_map<int,int>& slot_balances) const {
    auto sit = sessions_.find(lobby_id);
    if (sit == sessions_.end()) return -1;

    const BotEntry* best = nullptr;
    for (const auto& entry : sit->second) {
        if (entry.slot < 0) continue;  // unassigned
        if (!best) { best = &entry; continue; }

        const int e_diff  = static_cast<int>(entry.difficulty);
        const int b_diff  = static_cast<int>(best->difficulty);
        if (e_diff < b_diff) { best = &entry; continue; }
        if (e_diff > b_diff) continue;

        // Same difficulty — pick the one with less cash.
        const int e_cash = [&]{ auto it = slot_balances.find(entry.slot); return it != slot_balances.end() ? it->second : 0; }();
        const int b_cash = [&]{ auto it = slot_balances.find(best->slot);  return it != slot_balances.end() ? it->second : 0; }();
        if (e_cash < b_cash) best = &entry;
    }
    return best ? best->slot : -1;
}

void BotManager::spawn_replacement_with_hand(
        const std::string&                       lobby_id,
        int                                      slot_index,
        const std::array<int,4>&                 hand,
        const std::string&                       difficulty,
        BotSpawnContext                          ctx,
        moodycamel::ReaderWriterQueue<NetEvent>& session_inbound,
        bool                                     is_ui_mode) {
    ctx.slot           = slot_index;
    ctx.hand           = hand;
    ctx.difficulty_str = difficulty;
    spawn_replacement(lobby_id, ctx, session_inbound, is_ui_mode);
}

} // namespace anjeer::server
