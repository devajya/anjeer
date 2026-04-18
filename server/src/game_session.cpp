#include "server/game_session.h"
#include "server/game_session_wire.h"

#include <chrono>
#include <ctime>
#include <string>
#include <thread>
#include <variant>

namespace anjeer::server {

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

struct ScoringInputs {
    std::vector<engine::PlayerHand> hands;
    std::vector<bool>               disconnected;
};

struct SuitSideResult {
    engine::Suit suit;
    engine::Side side;
};

static void log_recv(int32_t player_slot, std::string_view msg, Logger& slog) {
    constexpr std::size_t kMaxRawLog = 256;
    std::string raw(msg.substr(0, kMaxRawLog));
    for (auto& c : raw) if (c == '\n' || c == '\r') c = ' ';
    slog.info("recv",
              "player=" + std::to_string(player_slot) +
              " raw=" + raw +
              (msg.size() > kMaxRawLog ? "…" : ""));
}

// ─────────────────────────────────────────────────────────────────────────────
// GameSession implementation
// ─────────────────────────────────────────────────────────────────────────────

GameSession::GameSession(
    std::array<engine::OrderBook, 4> books,
    std::array<bool, 4>              active_suits,
    const ServerConfig&              cfg,
    Logger&                          server_log,
    Logger&                          engine_log,
    uWS::Loop*                       loop,
    std::mt19937&                    rng)
    : cfg_(cfg)
    , server_log_(server_log)
    , engine_log_(engine_log)
    , loop_(loop)
    , rng_(rng)
    , books_(std::move(books))
    , active_suits_(active_suits)
{
    player_slots_.resize(cfg_.game.player_count, nullptr);
    // AGENT-CTX: Balances initialised once at session start, not per-connect.
    // A balance survives a player leaving and rejoining within the same session.
    available_cash_.resize(cfg_.game.player_count, cfg_.scoring.starting_balance);
}

GameSession::~GameSession() {
    // Defensive: shutdown() should be called explicitly before destruction,
    // but guard here in case of exception paths.
    if (running_) running_ = false;
    if (countdown_thread_.has_value()    && countdown_thread_->joinable())
        countdown_thread_->join();
    if (round_timer_thread_.has_value()  && round_timer_thread_->joinable())
        round_timer_thread_->join();
}

void GameSession::shutdown() {
    running_ = false;
    if (countdown_thread_.has_value()   && countdown_thread_->joinable())
        countdown_thread_->join();
    if (round_timer_thread_.has_value() && round_timer_thread_->joinable())
        round_timer_thread_->join();
}

// ─────────────────────────────────────────────────────────────────────────────
// Event processing pipeline
// ─────────────────────────────────────────────────────────────────────────────

// AGENT-CTX: Serialises engine events and sends to the appropriate sockets.
// No game-state mutation — does not clear books. Returns had_trade so
// apply_engine_events can decide whether to wipe. BookUpdateEvent is suppressed
// when a trade occurred in the same batch because the wipe broadcasts null
// book_updates immediately after.
bool GameSession::dispatch_events(
        WsHandle ws,
        const std::vector<engine::OrderEvent>& events) {
    bool had_trade = false;

    for (const auto& ev : events) {

        if (const auto* ack = std::get_if<engine::OrderAckEvent>(&ev)) {
            const std::string payload = nlohmann::json{
                {"type",     "order_ack"},
                {"order_id", ack->order_id},
                {"suit",     ack->suit},
                {"side",     serialise::side(ack->side)},
                {"price",    ack->price},
            }.dump();
            ws->send(payload, uWS::OpCode::TEXT);
            server_log_.info("send",
                      "player=" + std::to_string(ws->getUserData()->player_slot) +
                      " type=order_ack order_id=" + std::to_string(ack->order_id) +
                      " suit=" + ack->suit +
                      " side=" + serialise::side(ack->side) +
                      " price=" + std::to_string(ack->price));
            engine_log_.info("order_ack",
                      "order_id=" + std::to_string(ack->order_id) +
                      " player=" + std::to_string(ws->getUserData()->player_slot) +
                      " suit=" + ack->suit +
                      " side=" + serialise::side(ack->side) +
                      " price=" + std::to_string(ack->price));
        }

        else if (const auto* t = std::get_if<engine::TradeEvent>(&ev)) {
            engine_log_.info("trade",
                      "suit=" + t->suit +
                      " price=" + std::to_string(t->price) +
                      " aggressor=" + serialise::side(t->aggressor_side) +
                      " buyer_id=" + std::to_string(t->buyer_id) +
                      " seller_id=" + std::to_string(t->seller_id));
            serialise::trade(connections_, *t, server_log_);
            had_trade = true;
        }

        else if (const auto* upd = std::get_if<engine::BookUpdateEvent>(&ev)) {
            if (!had_trade) {
                serialise::book_update(connections_, upd->suit,
                                       upd->best_bid, upd->best_ask, server_log_);
            } else {
                engine_log_.debug("book_update",
                           "suppressed (trade in same batch) suit=" + upd->suit);
            }
        }

        else if (const auto* cack = std::get_if<engine::OrderCancelAckEvent>(&ev)) {
            const std::string payload = nlohmann::json{
                {"type",     "order_cancel_ack"},
                {"order_id", cack->order_id},
            }.dump();
            ws->send(payload, uWS::OpCode::TEXT);
            server_log_.info("send",
                      "player=" + std::to_string(ws->getUserData()->player_slot) +
                      " type=order_cancel_ack order_id=" + std::to_string(cack->order_id));
            engine_log_.info("cancel_ack",
                      "order_id=" + std::to_string(cack->order_id) +
                      " player=" + std::to_string(ws->getUserData()->player_slot));
        }

        else if (const auto* err = std::get_if<engine::OrderErrorEvent>(&ev)) {
            engine_log_.warn("engine_error",
                      "code=" + serialise::error_code_str(to_ws_error_code(err->code)) +
                      " message=" + err->message +
                      " player=" + std::to_string(ws->getUserData()->player_slot));
            serialise::error(ws, to_ws_error_code(err->code), err->message, server_log_);
        }
    }

    return had_trade;
}

// AGENT-CTX: Global wipe mechanic (Slice 2 resolution 1).
// Any executed trade resets ALL suit order books simultaneously.
// Separated from dispatch_events so serialise (event→JSON/socket) and
// game-mechanic policy (wipe books on trade) are independently testable.
void GameSession::apply_global_wipe() {
    engine_log_.info("global_wipe", "wiping all books after trade");
    for (auto s : engine::kAllSuits) {
        const int si = engine::suit_index(s);
        if (!active_suits_[si]) continue;
        const auto events = books_[si].wipe();
        for (const auto& ev : events) {
            if (const auto* upd = std::get_if<engine::BookUpdateEvent>(&ev)) {
                serialise::book_update(connections_, upd->suit,
                                       upd->best_bid, upd->best_ask, server_log_);
            }
        }
    }
}

// AGENT-CTX: Called after every dispatch_events() that produced a trade.
// game_state_ may be null during tests or if somehow called before deal;
// guard is intentional — the server should not crash on a double-fire.
void GameSession::apply_card_transfers(
        const std::vector<engine::OrderEvent>& events) {
    if (!game_state_) return;
    for (const auto& ev : events) {
        if (const auto* t = std::get_if<engine::TradeEvent>(&ev)) {
            const auto suit_opt = engine::suit_from_string(t->suit);
            if (!suit_opt) continue;
            game_state_->transfer_card(t->seller_id, t->buyer_id, *suit_opt);
            engine_log_.info("transfer_card",
                      "seller=" + std::to_string(t->seller_id) +
                      " buyer="  + std::to_string(t->buyer_id)  +
                      " suit="   + t->suit);
        }
    }
}

void GameSession::apply_trade_settlements(
        const std::vector<engine::OrderEvent>& events) {
    for (const auto& ev : events) {
        if (const auto* t = std::get_if<engine::TradeEvent>(&ev)) {
            available_cash_[t->buyer_id]  -= t->price;
            available_cash_[t->seller_id] += t->price;
            engine_log_.info("trade_settlement",
                      "buyer="   + std::to_string(t->buyer_id)  +
                      " cash-=" + std::to_string(t->price) +
                      " seller=" + std::to_string(t->seller_id) +
                      " cash+=" + std::to_string(t->price));
            for (int party : {t->buyer_id, t->seller_id}) {
                if (auto* ws = player_slots_[party]) {
                    const std::string payload = nlohmann::json{
                        {"type",    "balance_update"},
                        {"balance", available_cash_[party]},
                    }.dump();
                    ws->send(payload, uWS::OpCode::TEXT);
                    server_log_.info("send",
                              "player=" + std::to_string(party) +
                              " type=balance_update balance=" +
                              std::to_string(available_cash_[party]));
                }
            }
        }
    }
}

void GameSession::apply_engine_events(
        WsHandle ws,
        const std::vector<engine::OrderEvent>& events) {
    const bool had_trade = dispatch_events(ws, events);
    apply_card_transfers(events);
    apply_trade_settlements(events);
    if (had_trade) apply_global_wipe();
}

// ─────────────────────────────────────────────────────────────────────────────
// validate_suit_and_side — shared by handle_submit and handle_nudge
// ─────────────────────────────────────────────────────────────────────────────
static std::optional<SuitSideResult> validate_suit_and_side(
        WsHandle                      ws,
        const std::array<bool, 4>&    active_suits,
        const std::string&            suit_str,
        const std::string&            side_raw,
        std::string_view              op_name,
        Logger&                       server_log,
        Logger&                       engine_log) {
    const auto suit_opt = engine::suit_from_string(suit_str);
    if (!suit_opt || !active_suits[engine::suit_index(*suit_opt)]) {
        std::string known;
        for (auto s : engine::kAllSuits) {
            if (active_suits[engine::suit_index(s)])
                known += std::string(engine::suit_name(s)) + " ";
        }
        engine_log.warn(std::string(op_name),
                        "UNKNOWN_SUIT: '" + suit_str + "'  known suits: " + known);
        serialise::error(ws, WsErrorCode::UnknownSuit,
                         "suit '" + suit_str + "' is not active", server_log);
        return std::nullopt;
    }

    const auto side_opt = parse::side(side_raw);
    if (!side_opt) {
        engine_log.warn(std::string(op_name),
                        "bad side value: '" + side_raw + "'");
        serialise::error(ws, WsErrorCode::MalformedMessage,
                         "'side' must be \"buy\" or \"sell\"", server_log);
        return std::nullopt;
    }

    return SuitSideResult{*suit_opt, *side_opt};
}

// ─────────────────────────────────────────────────────────────────────────────
// Message handlers
// ─────────────────────────────────────────────────────────────────────────────

void GameSession::handle_submit(WsHandle ws, const nlohmann::json& j, int32_t player_slot) {
    if (round_phase_ != RoundPhase::Active) {
        serialise::error(ws, WsErrorCode::RoundNotActive, "round is not active", server_log_);
        return;
    }

    const auto fields = parse::submit_order(j);
    if (!fields) {
        server_log_.error("submit_order",
                   "player=" + std::to_string(player_slot) + " missing required fields");
        serialise::error(ws, WsErrorCode::MalformedMessage,
                         "submit_order requires 'suit', 'side', 'price'", server_log_);
        return;
    }

    engine_log_.info("submit_order",
              "player=" + std::to_string(player_slot) +
              " suit=" + fields->suit +
              " side=" + fields->side +
              " price=" + std::to_string(fields->price));

    const auto validated = validate_suit_and_side(
        ws, active_suits_, fields->suit, fields->side, "submit_order",
        server_log_, engine_log_);
    if (!validated) return;

    if (validated->side == engine::Side::Buy &&
        fields->price > available_cash_[player_slot]) {
        serialise::error(ws, WsErrorCode::InsufficientBalance,
                         "insufficient balance: have " +
                         std::to_string(available_cash_[player_slot]) +
                         ", need " + std::to_string(fields->price), server_log_);
        return;
    }

    engine_log_.debug("submit_order",
               "calling engine — suit=" + fields->suit +
               " player=" + std::to_string(player_slot) +
               " side=" + fields->side +
               " price=" + std::to_string(fields->price));
    auto events = books_[engine::suit_index(validated->suit)]
                      .submit(player_slot, validated->side, fields->price);
    engine_log_.info("submit_order",
              "engine returned " + std::to_string(events.size()) + " event(s)");

    apply_engine_events(ws, events);
}

void GameSession::handle_nudge(WsHandle ws, const nlohmann::json& j, int32_t player_slot) {
    if (round_phase_ != RoundPhase::Active) {
        serialise::error(ws, WsErrorCode::RoundNotActive, "round is not active", server_log_);
        return;
    }

    const auto fields = parse::nudge(j);
    if (!fields) {
        server_log_.error("nudge",
                   "player=" + std::to_string(player_slot) + " missing required fields");
        serialise::error(ws, WsErrorCode::MalformedMessage,
                         "nudge requires 'suit' and 'side'", server_log_);
        return;
    }

    engine_log_.info("nudge",
              "player=" + std::to_string(player_slot) +
              " suit=" + fields->suit +
              " side=" + fields->side);

    const auto validated = validate_suit_and_side(
        ws, active_suits_, fields->suit, fields->side, "nudge",
        server_log_, engine_log_);
    if (!validated) return;

    auto events = books_[engine::suit_index(validated->suit)]
                      .nudge(validated->side, player_slot);
    engine_log_.info("nudge",
              "engine returned " + std::to_string(events.size()) + " event(s)");

    apply_engine_events(ws, events);
}

void GameSession::handle_cancel(WsHandle ws, const nlohmann::json& j, int32_t player_slot) {
    if (round_phase_ != RoundPhase::Active) {
        serialise::error(ws, WsErrorCode::RoundNotActive, "round is not active", server_log_);
        return;
    }

    const auto fields = parse::cancel_order(j);
    if (!fields) {
        server_log_.error("cancel_order",
                   "player=" + std::to_string(player_slot) + " missing required fields");
        serialise::error(ws, WsErrorCode::MalformedMessage,
                         "cancel_order requires 'order_id' (int)", server_log_);
        return;
    }

    engine_log_.info("cancel_order",
              "player=" + std::to_string(player_slot) +
              " order_id=" + std::to_string(fields->order_id));

    // AGENT-CTX: Cancel searches ALL books because the wire protocol omits
    // suit from the cancel message.
    bool handled = false;
    for (auto s : engine::kAllSuits) {
        const int si = engine::suit_index(s);
        if (!active_suits_[si]) continue;
        auto events = books_[si].cancel(fields->order_id, player_slot);
        if (events.empty()) continue;
        const auto* err = std::get_if<engine::OrderErrorEvent>(&events.front());
        if (err && err->code == engine::OrderErrorEvent::Code::OrderNotFound) continue;

        handled = true;
        if (dispatch_events(ws, events))
            apply_global_wipe();
        break;
    }

    if (!handled) {
        engine_log_.warn("cancel_order",
                  "order_id=" + std::to_string(fields->order_id) + " not found in any book");
        serialise::error(ws, WsErrorCode::OrderNotFound,
                         "order " + std::to_string(fields->order_id) + " not found", server_log_);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// handle_start_game — explicit trigger replacing the N-connections auto-start
// ─────────────────────────────────────────────────────────────────────────────

// AGENT-CTX: Silently ignores the request if conditions aren't met rather than
// sending an error. The frontend button is hidden until conditions are met, so
// a start_game when not ready means a stale/racing message — ignore is correct.
void GameSession::handle_start_game() {
    if (round_phase_ != RoundPhase::Waiting) {
        server_log_.warn("start_game", "ignored — round not in Waiting phase");
        return;
    }
    if (countdown_in_progress_) {
        server_log_.warn("start_game", "ignored — countdown already in progress");
        return;
    }
    if (connected_count_ < cfg_.game.player_count) {
        server_log_.warn("start_game",
            "ignored — not enough players: " +
            std::to_string(connected_count_) + "/" +
            std::to_string(cfg_.game.player_count));
        return;
    }
    server_log_.info("start_game", "received — beginning countdown");
    begin_countdown();
}

// Slice 6 replaces this with scoped lobby_joined / lobby_left events.
void GameSession::broadcast_waiting_for_start() {
    if (countdown_in_progress_ || round_phase_ != RoundPhase::Waiting) return;
    const std::string payload = nlohmann::json{
        {"type",      "waiting_for_start"},
        {"connected", connected_count_},
        {"required",  cfg_.game.player_count},
    }.dump();
    for (WsHandle ws : connections_)
        ws->send(payload, uWS::OpCode::TEXT);
    server_log_.info("broadcast",
        "type=waiting_for_start connected=" + std::to_string(connected_count_) +
        "/" + std::to_string(cfg_.game.player_count));
}

// ─────────────────────────────────────────────────────────────────────────────
// Round lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void GameSession::handle_round_expiry() {
    // AGENT-CTX: Null is impossible today (timer only fires after deal), but
    // Slice 6 lobby handoff can destroy game_state_ between timer start and
    // fire. Guard prevents crash.
    if (!game_state_) {
        server_log_.error("round_expiry", "game_state is null — expiry handler aborted");
        return;
    }

    round_phase_ = RoundPhase::Scoring;
    apply_global_wipe();

    ScoringInputs in;
    in.hands.reserve(cfg_.game.player_count);
    in.disconnected.reserve(cfg_.game.player_count);
    for (int p = 0; p < cfg_.game.player_count; ++p) {
        in.hands.push_back(game_state_->hand(p));
        in.disconnected.push_back(player_slots_[p] == nullptr);
    }

    const engine::ScoringConfig sc{
        cfg_.scoring.buy_in,
        cfg_.scoring.points_per_card,
    };
    const auto result = engine::score_round(
        in.hands,
        game_state_->goal_suit(),
        in.disconnected,
        sc);

    engine_log_.info("score_round",
        "goal_suit=" + std::string(engine::suit_name(result.goal_suit)) +
        " pot="        + std::to_string(result.pot) +
        " bonus_pool=" + std::to_string(result.bonus_pool));

    for (int p = 0; p < cfg_.game.player_count; ++p)
        available_cash_[p] += result.player_results[p].payout;

    const std::string re_payload =
        serialise::round_end_payload(result, available_cash_);
    for (int p = 0; p < cfg_.game.player_count; ++p) {
        WsHandle ws = player_slots_[p];
        if (!ws) {
            server_log_.warn("round_end",
                "slot=" + std::to_string(p) + " disconnected — round_end not sent");
            continue;
        }
        ws->send(re_payload, uWS::OpCode::TEXT);
        server_log_.info("send",
            "player=" + std::to_string(p) + " type=round_end" +
            " payout="  + std::to_string(result.player_results[p].payout) +
            " balance=" + std::to_string(available_cash_[p]));
    }

    round_phase_ = RoundPhase::Ended;
    server_log_.info("round", "round ended");
}

void GameSession::handle_deal_and_start() {
    if (!running_ || round_started_) return;
    round_started_ = true;
    round_phase_   = RoundPhase::Active;

    engine::GameState::Config gs_cfg{
        cfg_.game.player_count,
        cfg_.game.total_cards,
        cfg_.game.card_distribution,
    };
    game_state_ = std::make_unique<engine::GameState>(gs_cfg);
    auto deal = game_state_->deal(rng_);

    if (deal.uneven_deal) {
        for (int p = 0; p < static_cast<int>(deal.hands.size()); ++p) {
            if (deal.hands[p].has_extra_card) {
                server_log_.warn("round",
                    "UNEVEN_DEAL slot=" + std::to_string(p) +
                    " has informational edge — see future EV module.");
            }
        }
    }

    engine_log_.info("deal",
        "goal_suit=" + std::string(engine::suit_name(deal.goal_suit)) +
        " totals[clubs="    + std::to_string(deal.suit_totals[engine::suit_index(engine::Suit::Clubs)])    +
        " diamonds=" + std::to_string(deal.suit_totals[engine::suit_index(engine::Suit::Diamonds)]) +
        " hearts="   + std::to_string(deal.suit_totals[engine::suit_index(engine::Suit::Hearts)])   +
        " spades="   + std::to_string(deal.suit_totals[engine::suit_index(engine::Suit::Spades)])   + "]");

    // Compute round_end_at once; same value sent to all players so clients
    // derive the same deadline without clock-skew between messages.
    auto re_now = std::chrono::system_clock::now();
    auto re_at  = re_now + std::chrono::seconds(cfg_.game.round_duration_seconds);
    auto re_t   = std::chrono::system_clock::to_time_t(re_at);
    std::tm re_gmt{};
    gmtime_r(&re_t, &re_gmt);
    char re_buf[32];
    std::strftime(re_buf, sizeof(re_buf), "%Y-%m-%dT%H:%M:%S.000Z", &re_gmt);
    const std::string round_end_at_str(re_buf);

    for (int slot = 0; slot < cfg_.game.player_count; ++slot) {
        WsHandle ws = player_slots_[slot];
        if (!ws) {
            server_log_.warn("round",
                "slot=" + std::to_string(slot) +
                " disconnected during countdown — hand not sent");
            continue;
        }
        if (available_cash_[slot] < cfg_.scoring.buy_in) {
            server_log_.warn("round",
                "slot=" + std::to_string(slot) + " insufficient balance for buy-in" +
                " (available=" + std::to_string(available_cash_[slot]) +
                " buy_in=" + std::to_string(cfg_.scoring.buy_in) + ")");
        }
        available_cash_[slot] -= cfg_.scoring.buy_in;
        ws->send(serialise::round_start_payload(
                     slot, deal.hands[slot], round_end_at_str, available_cash_[slot]),
                 uWS::OpCode::TEXT);
        server_log_.info("send",
            "player=" + std::to_string(slot) + " type=round_start" +
            " round_end_at=" + round_end_at_str +
            " clubs="    + std::to_string(deal.hands[slot].suit_counts[0]) +
            " diamonds=" + std::to_string(deal.hands[slot].suit_counts[1]) +
            " hearts="   + std::to_string(deal.hands[slot].suit_counts[2]) +
            " spades="   + std::to_string(deal.hands[slot].suit_counts[3]));
    }

    server_log_.info("round",
        "round started — goal_suit=" +
        std::string(engine::suit_name(deal.goal_suit)) +
        " round_end_at=" + round_end_at_str +
        " (goal_suit withheld from clients until round end)");

    // AGENT-CTX: Capture `this` not by reference to individual members.
    // GameSession outlives the thread — shutdown() joins before destruction.
    round_timer_thread_.emplace([this]() {
        std::this_thread::sleep_for(
            std::chrono::seconds(cfg_.game.round_duration_seconds));
        if (!running_) return;
        loop_->defer([this]() {
            handle_round_expiry();
        });
    });
}

void GameSession::begin_countdown() {
    countdown_in_progress_ = true;

    // AGENT-CTX: Absolute ISO 8601 UTC timestamp so API clients can compute
    // remaining time regardless of when they receive the message.
    auto now     = std::chrono::system_clock::now();
    auto fire_at = now + std::chrono::seconds(cfg_.game.countdown_seconds);
    auto fire_t  = std::chrono::system_clock::to_time_t(fire_at);
    std::tm gmt{};
    gmtime_r(&fire_t, &gmt);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &gmt);
    const std::string starts_at_str(buf);

    const std::string payload = nlohmann::json{
        {"type",         "round_starting"},
        {"starts_at",    starts_at_str},
        {"player_count", cfg_.game.player_count},
    }.dump();
    for (WsHandle ws : connections_)
        ws->send(payload, uWS::OpCode::TEXT);
    server_log_.info("round",
                    "round_starting broadcast starts_at=" + starts_at_str +
                    " player_count=" + std::to_string(cfg_.game.player_count));

    countdown_thread_.emplace([this]() {
        std::this_thread::sleep_for(std::chrono::seconds(cfg_.game.countdown_seconds));
        if (!running_) return;
        loop_->defer([this]() {
            handle_deal_and_start();
        });
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// Connection callbacks
// ─────────────────────────────────────────────────────────────────────────────

void GameSession::on_connect(WsHandle ws) {
    int slot = -1;
    for (int i = 0; i < static_cast<int>(player_slots_.size()); ++i) {
        if (!player_slots_[i]) { slot = i; break; }
    }
    if (slot == -1) {
        serialise::error(ws, WsErrorCode::ServerFull,
                         "no player slots available", server_log_);
        ws->close();
        server_log_.warn("open", "connection rejected — all slots filled");
        return;
    }

    player_slots_[slot] = ws;
    connected_count_++;
    ws->getUserData()->player_slot = static_cast<int32_t>(slot);
    connections_.insert(ws);
    server_log_.info("open",
                    "slot=" + std::to_string(slot) +
                    " connected=" + std::to_string(connected_count_) +
                    "/" + std::to_string(cfg_.game.player_count));

    ws->send(nlohmann::json{{"type","player_hello"},{"player_id",slot}}.dump(),
             uWS::OpCode::TEXT);
    server_log_.info("send",
                    "player=" + std::to_string(slot) +
                    " type=player_hello player_id=" + std::to_string(slot));

    for (auto s : engine::kAllSuits) {
        const int si = engine::suit_index(s);
        if (!active_suits_[si]) continue;
        const auto& book     = books_[si];
        const std::string ss = std::string(engine::suit_name(s));
        ws->send(serialise::book_update_payload(ss, book.best_bid(), book.best_ask()),
                 uWS::OpCode::TEXT);
        server_log_.info("send",
                        "player=" + std::to_string(slot) +
                        " type=book_update(on-connect) suit=" + ss +
                        " best_bid=" + serialise::opt_price(book.best_bid()) +
                        " best_ask=" + serialise::opt_price(book.best_ask()));
    }

    broadcast_waiting_for_start();
}

void GameSession::on_message(WsHandle ws, std::string_view msg, uWS::OpCode op) {
    if (op != uWS::OpCode::TEXT) {
        server_log_.warn("recv", "ignored non-TEXT frame");
        return;
    }

    const int32_t player_slot = ws->getUserData()->player_slot;
    log_recv(player_slot, msg, server_log_);

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(msg);
    } catch (const nlohmann::json::exception& ex) {
        server_log_.error("recv",
                         "player=" + std::to_string(player_slot) +
                         " JSON parse failed: " + ex.what());
        serialise::error(ws, WsErrorCode::MalformedMessage, "invalid JSON", server_log_);
        return;
    }

    std::string type;
    try {
        type = j.at("type").get<std::string>();
    } catch (const nlohmann::json::exception& ex) {
        server_log_.error("recv",
                         "player=" + std::to_string(player_slot) +
                         " missing 'type': " + ex.what());
        serialise::error(ws, WsErrorCode::MalformedMessage,
                         "missing or invalid 'type' field", server_log_);
        return;
    }

    if      (type == "submit_order") handle_submit    (ws, j, player_slot);
    else if (type == "nudge")        handle_nudge     (ws, j, player_slot);
    else if (type == "cancel_order") handle_cancel    (ws, j, player_slot);
    else if (type == "start_game")   handle_start_game();
    else {
        server_log_.warn("recv",
                        "player=" + std::to_string(player_slot) +
                        " unknown type: '" + type + "'");
        serialise::error(ws, WsErrorCode::MalformedMessage,
                         "unknown message type: '" + type + "'", server_log_);
    }
}

void GameSession::on_close(WsHandle ws, int code, std::string_view /*reason*/) {
    const int32_t slot = ws->getUserData()->player_slot;
    connections_.erase(ws);
    if (slot >= 0 && slot < static_cast<int32_t>(player_slots_.size())) {
        player_slots_[slot] = nullptr;
        if (!round_started_) connected_count_--;
    }
    server_log_.info("close",
                    "slot=" + std::to_string(slot) +
                    " code=" + std::to_string(code) +
                    " total=" + std::to_string(connections_.size()));
    broadcast_waiting_for_start();
}

} // namespace anjeer::server
