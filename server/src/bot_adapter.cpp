#include "server/bot_adapter.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

namespace anjeer::server {

BotAdapter::BotAdapter(
        std::unique_ptr<anjeer::engine::BotAgent> strategy,
        const BotSpawnContext& ctx,
        int sim_delay_ms,
        int tick_interval_ms,
        int tick_jitter_ms,
        int thinking_min_ms,
        int thinking_max_ms,
        int max_concurrent_orders,
        std::string bot_uuid,
        std::string log_path)
    : strategy_(std::move(strategy))
    , player_slot_(ctx.slot)
    , bot_uuid_(std::move(bot_uuid))
    , sim_delay_ms_(sim_delay_ms)
    , tick_interval_ms_(tick_interval_ms)
    , tick_jitter_ms_(tick_jitter_ms)
    , thinking_min_ms_(thinking_min_ms)
    , thinking_max_ms_(thinking_max_ms)
    , max_concurrent_orders_(max_concurrent_orders)
    , points_per_card_(ctx.points_per_card)
    , buy_in_(ctx.buy_in)
    , round_duration_s_(ctx.round_duration_s)
    , adapter_rng_(static_cast<uint64_t>(ctx.slot) ^ 0x9e3779b97f4a7c15ULL)
{
    snapshot_.my_slot          = ctx.slot;
    snapshot_.points_per_card  = ctx.points_per_card;
    snapshot_.buy_in           = ctx.buy_in;
    snapshot_.round_duration_s = static_cast<float>(ctx.round_duration_s);

    // Draw initial decide interval (with jitter if non-zero).
    if (tick_jitter_ms_ > 0) {
        std::uniform_int_distribution<int> jitter(-tick_jitter_ms_, tick_jitter_ms_);
        next_decide_interval_ms_ = std::max(0, tick_interval_ms_ + jitter(adapter_rng_));
    } else {
        next_decide_interval_ms_ = tick_interval_ms_;
    }

    if (!log_path.empty()) {
        log_.open(log_path, std::ios::app);
        if (log_.is_open())
            log_ << "\n=== " << strategy_->name() << " slot=" << player_slot_
                 << " started " << now_str() << " ===\n";
    }
}

void BotAdapter::teardown() {
    alive_.store(false, std::memory_order_release);
}

// ── uWS drain-loop thread ─────────────────────────────────────────────────────

void BotAdapter::on_game_event(std::string_view json_payload, bool /*is_targeted*/) {
    if (!alive_.load(std::memory_order_relaxed)) return;
    event_queue_.enqueue(std::string(json_payload));
}

void BotAdapter::drain_to_session(moodycamel::ReaderWriterQueue<NetEvent>& session_inbound) {
    if (!alive_.load(std::memory_order_relaxed)) return;
    NetEvent ev;
    while (action_queue_.try_dequeue(ev))
        session_inbound.enqueue(std::move(ev));
}

// ── BotScheduler tick thread ──────────────────────────────────────────────────

void BotAdapter::tick() {
    if (!alive_.load(std::memory_order_acquire)) return;

    // Drain all pending game events first so strategy state is current.
    {
        std::string json;
        while (event_queue_.try_dequeue(json))
            process_event(json);
    }

    auto now = clock::now();

    // Drain expired pending actions into action_queue_.
    while (!pending_.empty() && pending_.front().first <= now) {
        if (alive_.load(std::memory_order_relaxed))
            action_queue_.enqueue(std::move(pending_.front().second));
        pending_.pop_front();
    }

    // Expire overdue in-flight deadlines.
    while (!flight_deadlines_.empty() && flight_deadlines_.front().deadline <= now) {
        actions_in_flight_ = std::max(0, actions_in_flight_ - 1);
        flight_deadlines_.pop_front();
    }

    if (!snapshot_.round_active) return;
    if (actions_in_flight_ >= max_concurrent_orders_) return;

    // Per-bot rate limiting.
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now - last_decide_at_).count();
    if (elapsed_ms < next_decide_interval_ms_) return;

    // Refresh time_remaining_s.
    if (round_end_valid_) {
        auto sys_now = std::chrono::system_clock::now();
        auto rem = std::chrono::duration_cast<std::chrono::duration<double>>(
            round_end_time_ - sys_now);
        snapshot_.time_remaining_s = std::max(0.0, rem.count());
    }

    auto actions = strategy_->decide(snapshot_);
    last_decide_at_ = now;

    // Draw next decide interval with jitter.
    if (tick_jitter_ms_ > 0) {
        std::uniform_int_distribution<int> jitter(-tick_jitter_ms_, tick_jitter_ms_);
        next_decide_interval_ms_ = std::max(0, tick_interval_ms_ + jitter(adapter_rng_));
    } else {
        next_decide_interval_ms_ = tick_interval_ms_;
    }

    log_tick(actions);

    // Submit up to remaining capacity.
    std::uniform_int_distribution<int> think(thinking_min_ms_, std::max(thinking_min_ms_, thinking_max_ms_));
    for (auto& action : actions) {
        if (actions_in_flight_ >= max_concurrent_orders_) break;
        if (std::holds_alternative<anjeer::engine::BotNoAction>(action)) continue;

        int delay    = sim_delay_ms_ + think(adapter_rng_);
        auto fire    = now + std::chrono::milliseconds(delay);

        if (std::holds_alternative<anjeer::engine::BotSubmitOrder>(action)) {
            const auto& sub = std::get<anjeer::engine::BotSubmitOrder>(action);
            actions_in_flight_++;
            flight_deadlines_.push_back({
                now + std::chrono::milliseconds(std::max(delay * 3, 300)),
                sub.suit, sub.side
            });
        }
        pending_.push_back({fire, action_to_net_event(action)});
    }
}

// ── Private helpers ───────────────────────────────────────────────────────────

void BotAdapter::process_event(const std::string& json) {
    nlohmann::json j;
    try { j = nlohmann::json::parse(json); } catch (...) { return; }

    auto it = j.find("type");
    if (it == j.end() || !it->is_string()) return;
    const std::string& type = it->get_ref<const std::string&>();

    if (type == "round_start") {
        snapshot_.round_active = true;
        actions_in_flight_     = 0;
        flight_deadlines_.clear();
        snapshot_.my_slot  = j.value("player_slot", player_slot_);
        snapshot_.balance          = j.value("balance", int32_t{0});

        if (j.contains("hand")) {
            const auto& h   = j["hand"];
            snapshot_.hand[0] = h.value("clubs",    0);
            snapshot_.hand[1] = h.value("diamonds", 0);
            snapshot_.hand[2] = h.value("hearts",   0);
            snapshot_.hand[3] = h.value("spades",   0);
        }

        if (j.contains("all_balances") && j["all_balances"].is_array()) {
            int pc = static_cast<int>(j["all_balances"].size());
            snapshot_.num_active_slots = pc;
        }

        if (j.contains("round_end_at") && j["round_end_at"].is_string()) {
            round_end_time_  = parse_iso(j["round_end_at"].get<std::string>());
            round_end_valid_ = true;
        }

        snapshot_.round_duration_s = static_cast<float>(round_duration_s_);
        snapshot_.best_bid.fill(std::nullopt);
        snapshot_.best_ask.fill(std::nullopt);
        snapshot_.best_bid_qty.fill(std::nullopt);
        snapshot_.best_ask_qty.fill(std::nullopt);
        snapshot_.last_trade_price.fill(std::nullopt);

        anjeer::engine::BotRoundStartEvent ev{};
        ev.hand            = snapshot_.hand;
        ev.round_duration_s = snapshot_.round_duration_s;
        ev.player_slot     = snapshot_.my_slot;
        ev.player_count    = snapshot_.num_active_slots;
        ev.balance         = snapshot_.balance;
        ev.buy_in          = buy_in_;
        ev.points_per_card = points_per_card_;
        strategy_->on_event(ev);
        log_event("round_start",
            "slot=" + std::to_string(snapshot_.my_slot) +
            " players=" + std::to_string(snapshot_.num_active_slots) +
            " hand=[C:" + std::to_string(snapshot_.hand[0]) +
            " D:" + std::to_string(snapshot_.hand[1]) +
            " H:" + std::to_string(snapshot_.hand[2]) +
            " S:" + std::to_string(snapshot_.hand[3]) + "]" +
            " balance=" + std::to_string(snapshot_.balance));
    }
    else if (type == "book_update") {
        auto suit_opt = anjeer::engine::suit_from_string(j.value("suit", ""));
        if (!suit_opt) return;
        int si = anjeer::engine::suit_index(*suit_opt);

        const auto& bid_j = j["best_bid"];
        const auto& ask_j = j["best_ask"];
        snapshot_.best_bid[si] = bid_j.is_number() ? std::optional<int32_t>(bid_j.get<int32_t>()) : std::nullopt;
        snapshot_.best_ask[si] = ask_j.is_number() ? std::optional<int32_t>(ask_j.get<int32_t>()) : std::nullopt;

        const auto bq_it = j.find("best_bid_qty");
        const auto aq_it = j.find("best_ask_qty");
        snapshot_.best_bid_qty[si] = (bq_it != j.end() && bq_it->is_number())
            ? std::optional<int32_t>(bq_it->get<int32_t>()) : std::nullopt;
        snapshot_.best_ask_qty[si] = (aq_it != j.end() && aq_it->is_number())
            ? std::optional<int32_t>(aq_it->get<int32_t>()) : std::nullopt;

        strategy_->on_event(anjeer::engine::BotBookUpdateEvent{
            *suit_opt,
            snapshot_.best_bid[si],  snapshot_.best_ask[si],
            snapshot_.best_bid_qty[si], snapshot_.best_ask_qty[si]});
    }
    else if (type == "trade") {
        auto suit_opt = anjeer::engine::suit_from_string(j.value("suit", ""));
        if (!suit_opt) return;

        std::optional<anjeer::engine::Side> your_side;
        if (j.contains("your_side") && j["your_side"].is_string()) {
            const auto& s = j["your_side"].get_ref<const std::string&>();
            if (s == "buy")  your_side = anjeer::engine::Side::Buy;
            if (s == "sell") your_side = anjeer::engine::Side::Sell;
        }

        // Any trade = global wipe — all resting orders gone; reset in-flight tracking.
        actions_in_flight_ = 0;
        flight_deadlines_.clear();
        snapshot_.last_trade_price[anjeer::engine::suit_index(*suit_opt)] =
            j.value("price", int32_t{0});

        // Update own hand immediately on fill so strategies see current inventory.
        if (your_side) {
            int si = anjeer::engine::suit_index(*suit_opt);
            if (*your_side == anjeer::engine::Side::Buy)
                snapshot_.hand[si]++;
            else
                snapshot_.hand[si] = std::max(0, snapshot_.hand[si] - 1);
        }

        strategy_->on_event(anjeer::engine::BotTradeEvent{
            *suit_opt, j.value("price", int32_t{0}), your_side});
        log_event("trade",
            std::string(anjeer::engine::suit_name(*suit_opt)) +
            "@" + std::to_string(j.value("price", int32_t{0})) +
            " your_side=" + (your_side ? (*your_side == anjeer::engine::Side::Buy ? "buy→hand+" : "sell→hand-") : "none") +
            " pending_cleared=yes");
    }
    else if (type == "delta_update") {
        if (!j.contains("deltas") || !j["deltas"].is_array()) return;
        const auto& deltas = j["deltas"];
        std::vector<std::array<int, 4>> table;
        table.reserve(deltas.size());
        for (const auto& row : deltas) {
            std::array<int, 4> arr{0, 0, 0, 0};
            if (row.is_array()) {
                for (int s = 0; s < 4 && s < static_cast<int>(row.size()); ++s)
                    arr[s] = row[s].get<int>();
            }
            table.push_back(arr);
        }
        int n_rows = static_cast<int>(table.size());
        strategy_->on_event(anjeer::engine::BotDeltaUpdateEvent{std::move(table)});
        log_event("delta_update", "rows=" + std::to_string(n_rows));
    }
    else if (type == "order_ack") {
        std::string oid = std::to_string(j.value("order_id", int64_t{0}));

        auto suit_opt = anjeer::engine::suit_from_string(j.value("suit", ""));
        if (!suit_opt) return;
        const std::string side_str = j.value("side", std::string{});
        anjeer::engine::Side side  = (side_str == "sell")
            ? anjeer::engine::Side::Sell : anjeer::engine::Side::Buy;
        int32_t price = j.value("price", int32_t{0});

        actions_in_flight_ = std::max(0, actions_in_flight_ - 1);
        if (!flight_deadlines_.empty()) flight_deadlines_.pop_front();

        strategy_->on_event(anjeer::engine::BotOrderAckEvent{
            oid, *suit_opt, side, price});
        log_event("order_ack",
            (side == anjeer::engine::Side::Buy ? "BUY " : "SELL ") +
            std::string(anjeer::engine::suit_name(*suit_opt)) +
            "@" + std::to_string(price) + " id=" + oid + " → pending set");
    }
    else if (type == "order_cancel_ack") {
        log_event("cancel_ack", "id=" + std::to_string(j.value("order_id", int64_t{0})));
    }
    else if (type == "error") {
        // Any order-related error (ORDER_NOT_FOUND, ORDER_QTY_NOT_SUPPORTED, etc.)
        // means our pending order no longer exists on the server — clear it so the
        // bot doesn't loop cancelling a ghost order indefinitely.
        const std::string code = j.value("code", std::string{});
        const bool order_gone = (code == "ORDER_NOT_FOUND" || code == "NOT_YOUR_ORDER");
        const bool submit_rejected = (code == "INSUFFICIENT_BALANCE" ||
                                      code == "INSUFFICIENT_CARDS"   ||
                                      code == "PRICE_OUT_OF_RANGE");
        if (order_gone) {
            log_event("error", code + " (order gone)");
        } else if (submit_rejected) {
            actions_in_flight_ = std::max(0, actions_in_flight_ - 1);
            if (!flight_deadlines_.empty()) {
                auto rec = flight_deadlines_.front();
                flight_deadlines_.pop_front();
                if (code == "INSUFFICIENT_CARDS" && rec.side == anjeer::engine::Side::Sell) {
                    int si = anjeer::engine::suit_index(rec.suit);
                    snapshot_.hand[si] = 0;
                    log_event("error", code + " → hand[" + std::to_string(si) + "]=0");
                } else {
                    log_event("error", code + " → in_flight decremented");
                }
            } else {
                log_event("error", code + " → in_flight decremented (no flight record)");
            }
        } else {
            log_event("error", code + " (ignored)");
        }
    }
    else if (type == "round_end") {
        snapshot_.round_active = false;
        round_end_valid_       = false;

        auto goal_opt = anjeer::engine::suit_from_string(j.value("goal_suit", ""));
        anjeer::engine::Suit goal = goal_opt ? *goal_opt : anjeer::engine::Suit::Clubs;

        int32_t final_balance = snapshot_.balance;
        if (j.contains("results") && j["results"].is_array()) {
            for (const auto& r : j["results"]) {
                if (r.value("player_slot", -1) == player_slot_)
                    final_balance = r.value("balance", final_balance);
            }
        }
        strategy_->on_event(anjeer::engine::BotRoundEndEvent{goal, final_balance});
        log_event("round_end",
            "goal=" + std::string(anjeer::engine::suit_name(goal)) +
            " final_balance=" + std::to_string(final_balance));
    }
    else if (type == "inter_round") {
        snapshot_.round_active = false;
        strategy_->on_event(anjeer::engine::BotInterRoundEvent{});
        log_event("inter_round", "round=" + std::to_string(j.value("round_number", 0)));
    }
    else if (type == "game_ended") {
        snapshot_.round_active = false;
        alive_.store(false, std::memory_order_release);
        strategy_->on_event(anjeer::engine::BotGameEndedEvent{});
    }
    else if (type == "all_balances") {
        if (j.contains("balances") && j["balances"].is_array()) {
            const auto& arr = j["balances"];
            if (player_slot_ < static_cast<int>(arr.size()))
                snapshot_.balance = arr[player_slot_].get<int32_t>();
        }
    }
    // hand_totals: ignored — bot tracks own hand via round_start + trade events
}

NetEvent BotAdapter::action_to_net_event(const anjeer::engine::BotAction& action) {
    if (const auto* sub = std::get_if<anjeer::engine::BotSubmitOrder>(&action)) {
        return NetSubmit{
            player_slot_,
            std::string(anjeer::engine::suit_name(sub->suit)),
            sub->side,
            sub->price,
            sub->qty
        };
    }
    if (const auto* can = std::get_if<anjeer::engine::BotCancelOrder>(&action)) {
        int64_t oid = 0;
        try { oid = std::stoll(can->order_id); } catch (...) {}
        return NetCancel{player_slot_, oid};
    }
    // BotNoAction — should not reach here (caller checks before pushing)
    return NetConnect{player_slot_, -1, "__bot__"};
}

// ── Logging helpers ───────────────────────────────────────────────────────────

std::string BotAdapter::now_str() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()) % 1000;
    std::ostringstream ss;
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    ss << std::put_time(&tm_buf, "%H:%M:%S") << '.'
       << std::setw(3) << std::setfill('0') << ms.count();
    return ss.str();
}

std::string BotAdapter::bid_ask_str(int si) const {
    std::ostringstream ss;
    if (snapshot_.best_bid[si]) ss << *snapshot_.best_bid[si]; else ss << "-";
    ss << "/";
    if (snapshot_.best_ask[si]) ss << *snapshot_.best_ask[si]; else ss << "-";
    return ss.str();
}

void BotAdapter::log_event(std::string_view label, std::string_view detail) {
    if (!log_.is_open()) return;
    log_ << now_str() << " EVENT " << label << "  " << detail << "\n";
}

void BotAdapter::log_tick(const std::vector<anjeer::engine::BotAction>& actions) {
    if (!log_.is_open()) return;
    static const char* snames[] = {"clubs","diamonds","hearts","spades"};

    log_ << now_str()
         << " TICK  t=" << std::fixed << std::setprecision(1) << snapshot_.time_remaining_s << "s"
         << "  round=" << (snapshot_.round_active ? "active" : "idle")
         << "  inflight=" << actions_in_flight_
         << "  book=[C:" << bid_ask_str(0) << " D:" << bid_ask_str(1)
         <<        " H:" << bid_ask_str(2) << " S:" << bid_ask_str(3) << "]\n";

    log_ << "  " << strategy_->debug_info(snapshot_) << "\n";

    for (const auto& action : actions) {
        if (std::holds_alternative<anjeer::engine::BotSubmitOrder>(action)) {
            const auto& s = std::get<anjeer::engine::BotSubmitOrder>(action);
            log_ << "  ACTION: SUBMIT " << (s.side == anjeer::engine::Side::Buy ? "BUY" : "SELL")
                 << " " << snames[anjeer::engine::suit_index(s.suit)] << "@" << s.price
                 << " qty=" << s.qty << "\n";
        } else if (std::holds_alternative<anjeer::engine::BotCancelOrder>(action)) {
            log_ << "  ACTION: CANCEL id="
                 << std::get<anjeer::engine::BotCancelOrder>(action).order_id << "\n";
        }
    }
    if (actions.empty() || (actions.size() == 1 && std::holds_alternative<anjeer::engine::BotNoAction>(actions[0])))
        log_ << "  ACTION: NOOP\n";
    log_.flush();
}

BotAdapter::sys_tp BotAdapter::parse_iso(const std::string& iso) {
    std::tm t{};
    const char* end = strptime(iso.c_str(), "%Y-%m-%dT%H:%M:%S", &t);
    if (!end) return std::chrono::system_clock::now();
    t.tm_isdst = 0;
    return std::chrono::system_clock::from_time_t(timegm(&t));
}

} // namespace anjeer::server
