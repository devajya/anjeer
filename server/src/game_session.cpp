#include "server/game_session.h"
#include "server/game_session_wire.h"
#include "server/market_data_wire.h"
#include "server/session_repo.h"
#include "server/eval/eval_types.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <string>
#include <thread>
#include <variant>

namespace anjeer::server {

//
// 12 fixed deck variants per the game specification. Each entry specifies
// per-suit card counts (indexed by engine::suit_index: clubs=0, diamonds=1,
// hearts=2, spades=3), the goal suit, and the bonus pool for that deck.
// Goal suits are explicit — 4 of 12 decks break the color_partner() rule so
// the goal suit cannot be derived from the distribution.
struct DeckDef {
    std::array<int, 4> distribution;
    engine::Suit       goal_suit;
};

static constexpr std::array<DeckDef, 12> kDecks = {{
    { {10,  8, 10, 12}, engine::Suit::Clubs    },
    { {10, 10,  8, 12}, engine::Suit::Clubs    },
    { { 8, 10, 10, 12}, engine::Suit::Clubs    },
    { {12, 10, 10,  8}, engine::Suit::Spades   },
    { {12,  8, 10, 10}, engine::Suit::Diamonds },
    { {12, 10,  8, 10}, engine::Suit::Spades   },
    { { 8, 10, 12, 10}, engine::Suit::Diamonds },
    { {10, 10, 12,  8}, engine::Suit::Spades   },
    { {10,  8, 12, 10}, engine::Suit::Diamonds },
    { {10, 12,  8, 10}, engine::Suit::Hearts   },
    { { 8, 12, 10, 10}, engine::Suit::Clubs    },
    { {10, 12, 10,  8}, engine::Suit::Spades   },
}};


static exchange::ExchangeSession build_exchange(const ServerConfig& cfg) {
    std::vector<exchange::InstrumentConfig> instruments;
    instruments.reserve(4);
    for (auto s : {engine::Suit::Clubs, engine::Suit::Diamonds,
                   engine::Suit::Hearts, engine::Suit::Spades}) {
        exchange::InstrumentConfig ic;
        ic.min_price                = cfg.order_book.min_price;
        ic.max_price                = cfg.order_book.max_price;
        ic.nudge_initial_buy_price  = cfg.order_book.nudge_initial_buy_price;
        ic.nudge_initial_sell_price = cfg.order_book.nudge_initial_sell_price;
        ic.label                    = std::string(engine::suit_name(s));
        instruments.push_back(ic);
    }
    return exchange::ExchangeSession(std::move(instruments));
}


GameSession::GameSession(
    std::string                                       session_id,
    std::string                                       lobby_id,
    std::vector<SlotInfo>                             slots,
    GameMode                                          game_mode,
    GameSessionContext                                ctx,
    moodycamel::ReaderWriterQueue<NetEvent>&          inbound,
    moodycamel::ReaderWriterQueue<GameEvent>&         outbound)
    : cfg_(ctx.cfg)
    , server_log_(ctx.server_log)
    , engine_log_(ctx.engine_log)
    , rng_(std::move(ctx.rng))
    , db_pool_(ctx.db_pool)
    , inbound_(inbound)
    , outbound_(outbound)
    , session_id_(std::move(session_id))
    , lobby_id_(std::move(lobby_id))
    , slots_(std::move(slots))
    , game_mode_(game_mode)
    , wipe_on_trade_(game_mode != GameMode::Advanced)
    , allow_multi_qty_(game_mode != GameMode::Simple)
    , exchange_(build_exchange(ctx.cfg))
{
    funded_this_round_.assign(slots_.size(), false);
    delta_table_.assign(slots_.size(), {0, 0, 0, 0});
    reconnect_deadlines_.assign(slots_.size(), std::nullopt);
    active_player_count_ = static_cast<int>(slots_.size());
    real_player_count_   = static_cast<int>(std::count_if(
        slots_.begin(), slots_.end(),
        [](const SlotInfo& s) { return s.player_id >= 0; }));

    for (const auto& suit_str : cfg_.order_book.active_suits) {
        const auto s = engine::suit_from_string(suit_str);
        if (s) active_suits_[engine::suit_index(*s)] = true;
    }

    eval_runner_ = std::make_unique<eval::EvalRunner>(
        std::move(ctx.eval_modules),
        [this](eval::EvalOutput out) {
            std::lock_guard<std::mutex> lk(eval_out_mu_);
            eval_out_pending_.push_back(std::move(out));
        });

    std::array<engine::DeckSpec, 12> deck_specs;
    for (int i = 0; i < static_cast<int>(kDecks.size()); ++i) {
        deck_specs[i].counts    = kDecks[i].distribution;
        deck_specs[i].goal_suit = kDecks[i].goal_suit;
    }
    eval_runner_->init_session(deck_specs);
    eval_runner_->start();
}

GameSession::~GameSession() {
    shutdown();
}

void GameSession::start() {
    game_loop_thread_ = std::thread([this]() { run(); });
}

void GameSession::shutdown() {
    stop_.store(true, std::memory_order_relaxed);
    if (game_loop_thread_.joinable()) game_loop_thread_.join();
    if (eval_runner_) eval_runner_->stop();
}

std::unordered_map<int,int> GameSession::slot_balances() const {
    std::unordered_map<int,int> result;
    for (int i = 0; i < static_cast<int>(slots_.size()); ++i)
        if (slots_[i].player_id != -1)
            result[i] = slots_[i].balance;
    return result;
}


void GameSession::run() {
    server_log_.info("session", "game loop started session=" + session_id_);
    try {
        while (!stop_.load(std::memory_order_relaxed)) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    } catch (const std::exception& ex) {
        server_log_.error("session", "unhandled exception: " + std::string(ex.what()));
        persist_error("unhandled_exception", ex.what());
        emit_broadcast(serialise::session_error_payload(ex.what()));
    } catch (...) {
        server_log_.error("session", "unknown exception in game loop");
        persist_error("unknown_exception", "unknown");
        emit_broadcast(serialise::session_error_payload("internal server error"));
    }
    outbound_.enqueue(GameDone{});
    done_.store(true, std::memory_order_release);
    server_log_.info("session", "game loop ended session=" + session_id_);
}

void GameSession::tick() {
    process_inbound();
    drain_eval_output();

    const auto now = std::chrono::steady_clock::now();

    switch (phase_) {
    case SessionPhase::Lobby:
        break;

    case SessionPhase::Countdown:
        if (now >= countdown_deadline_) begin_round();
        break;

    case SessionPhase::RoundActive:
        check_reconnect_expirations();
        if (now >= round_deadline_) {
            end_round();
        } else if (all_disconnected_ &&
                   now - all_disconnected_since_ >= kReconnectGrace) {
            server_log_.warn("session", "all disconnected for >500ms — ending game");
            end_game(true);
        }
        break;

    case SessionPhase::InterRound: {
        // Auto-start fires when the deadline passes — serves as a safety net
        // if the owner is disconnected. Owner can also trigger begin_round()
        // early via handle_owner_start_round (NetOwnerStartRound command).
        if (now >= inter_round_deadline_) {
            begin_round();
        }
        break;
    }

    case SessionPhase::Ended:
        stop_.store(true, std::memory_order_relaxed);
        break;
    }
}

void GameSession::process_inbound() {
    NetEvent ev;
    while (inbound_.try_dequeue(ev)) {
        std::visit([this](auto&& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr      (std::is_same_v<T, NetConnect>)    handle_connect(e);
            else if constexpr (std::is_same_v<T, NetDisconnect>) handle_disconnect(e);
            else if constexpr (std::is_same_v<T, NetSubmit>)     handle_submit(e);
            else if constexpr (std::is_same_v<T, NetNudge>)      handle_nudge(e);
            else if constexpr (std::is_same_v<T, NetCancel>)     handle_cancel(e);
            else if constexpr (std::is_same_v<T, NetStartGame>)  handle_start_game();
            else if constexpr (std::is_same_v<T, NetOwnerStartRound>) handle_owner_start_round();
            else if constexpr (std::is_same_v<T, NetOwnerEndGame>)   handle_owner_end_game();
            else if constexpr (std::is_same_v<T, NetPermanentLeave>) handle_permanent_leave(e.slot);
            else if constexpr (std::is_same_v<T, NetSpectatorJoin>)  handle_spectator_join(e);
            else if constexpr (std::is_same_v<T, NetSpectatorLeave>) handle_spectator_leave(e);
            else if constexpr (std::is_same_v<T, NetReconnectDisconnect>)
                handle_reconnect_disconnect(e.slot);
            else if constexpr (std::is_same_v<T, NetReconnectReattach>)
                handle_reconnect_reattach(e.slot, e.reconnect_token, e.reconnect_expires_at_ms, e.encoding);
            else if constexpr (std::is_same_v<T, NetAdmitQueue>)
                handle_admit_queue(e);
            else if constexpr (std::is_same_v<T, NetSendFeedSnapshot>)
                handle_send_feed_snapshot(e.slot, e.tier);
            else if constexpr (std::is_same_v<T, NetMarketDataConnect>)
                handle_market_data_connect(e.md_id);
        }, ev);
    }
}


void GameSession::handle_connect(const NetConnect& ev) {
    if (ev.slot < 0 || ev.slot >= static_cast<int32_t>(slots_.size())) {
        server_log_.warn("connect", "slot=" + std::to_string(ev.slot) + " out of range");
        return;
    }
    auto& slot = slots_[ev.slot];
    if (!slot.active) {
        // Slot re-used within grace window: re-activate
        slot.active = true;
        active_player_count_++;
    }
    slot.connected  = true;
    slot.player_id  = ev.player_id;
    slot.username   = ev.username;
    slot.encoding   = ev.encoding;
    all_disconnected_ = false;

    server_log_.info("connect",
        "slot=" + std::to_string(ev.slot) + " player=" + ev.username);

    // Send current book state to this slot
    for (auto s : engine::kAllSuits) {
        const int si = engine::suit_index(s);
        if (!active_suits_[si]) continue;
        emit_targeted(ev.slot, serialise::book_update_payload(
            std::string(engine::suit_name(s)),
            exchange_.best_bid(si), exchange_.best_ask(si),
            exchange_.current_seq(),
            exchange_.best_bid_slot(si), exchange_.best_ask_slot(si)));
    }

    if (phase_ == SessionPhase::Lobby) {
        broadcast_waiting_for_start();
    } else if (phase_ == SessionPhase::Countdown) {
        emit_targeted(ev.slot, nlohmann::json{
            {"type",         "round_starting"},
            {"starts_at",    steady_to_iso(countdown_deadline_)},
            {"player_count", static_cast<int>(slots_.size())},
        }.dump());
    }
}

void GameSession::handle_disconnect(const NetDisconnect& ev) {
    if (ev.slot < 0 || ev.slot >= static_cast<int32_t>(slots_.size())) return;
    auto& slot = slots_[ev.slot];
    slot.connected = false;

    server_log_.info("disconnect",
        "slot=" + std::to_string(ev.slot) + " player=" + slot.username);

    if (phase_ == SessionPhase::RoundActive || phase_ == SessionPhase::InterRound)
        emit_broadcast(serialise::game_player_left_payload(ev.slot, slot.username));

    bool any_connected = false;
    for (const auto& s : slots_) {
        if (s.active && s.connected) { any_connected = true; break; }
    }
    if (!any_connected && !all_disconnected_) {
        all_disconnected_       = true;
        all_disconnected_since_ = std::chrono::steady_clock::now();
    }

    check_end_condition();
}

void GameSession::handle_submit(const NetSubmit& ev) {
    if (phase_ != SessionPhase::RoundActive) {
        emit_error(ev.slot, "ROUND_NOT_ACTIVE", "round is not active");
        return;
    }
    const auto suit_opt = engine::suit_from_string(ev.suit);
    if (!suit_opt || !active_suits_[engine::suit_index(*suit_opt)]) {
        emit_error(ev.slot, "UNKNOWN_SUIT", "unknown suit: " + ev.suit);
        return;
    }
    if (ev.side == engine::Side::Buy && ev.price > slots_[ev.slot].balance) {
        emit_error(ev.slot, "INSUFFICIENT_BALANCE", "insufficient balance");
        return;
    }
    if (ev.side == engine::Side::Sell && game_state_) {
        int si = engine::suit_index(*suit_opt);
        if (game_state_->hand(ev.slot).suit_counts[si] == 0) {
            emit_error(ev.slot, "INSUFFICIENT_CARDS", "no cards of this suit to sell");
            return;
        }
    }
    const int si  = engine::suit_index(*suit_opt);
    const int qty = allow_multi_qty_ ? ev.qty : 1;
    auto result = exchange_.submit_order(
        static_cast<exchange::instrument_id_t>(si), ev.side, ev.price, ev.slot, qty);
    const bool had_trade = dispatch_result(ev.slot, result);
    if (had_trade) {
        apply_post_trade_state(result);
        if (wipe_on_trade_) apply_global_wipe();
    } else {
        push_book_updates_to_eval(result);
    }
}

void GameSession::handle_nudge(const NetNudge& ev) {
    if (phase_ != SessionPhase::RoundActive) {
        emit_error(ev.slot, "ROUND_NOT_ACTIVE", "round is not active");
        return;
    }
    const auto suit_opt = engine::suit_from_string(ev.suit);
    if (!suit_opt || !active_suits_[engine::suit_index(*suit_opt)]) {
        emit_error(ev.slot, "UNKNOWN_SUIT", "unknown suit: " + ev.suit);
        return;
    }
    const int si = engine::suit_index(*suit_opt);
    const auto bid = exchange_.best_bid(static_cast<exchange::instrument_id_t>(si));
    const auto ask = exchange_.best_ask(static_cast<exchange::instrument_id_t>(si));

    exchange::price_t nudge_price;
    if (ev.side == exchange::Side::Buy) {
        nudge_price = bid.has_value()
            ? std::min(bid.value() + 1, cfg_.order_book.max_price)
            : cfg_.order_book.nudge_initial_buy_price;
    } else {
        nudge_price = ask.has_value()
            ? std::max(ask.value() - 1, cfg_.order_book.min_price)
            : cfg_.order_book.nudge_initial_sell_price;
    }

    auto result = exchange_.submit_order(
        static_cast<exchange::instrument_id_t>(si), ev.side, nudge_price, ev.slot);
    const bool had_trade = dispatch_result(ev.slot, result);
    if (had_trade) {
        apply_post_trade_state(result);
        if (wipe_on_trade_) apply_global_wipe();
    } else {
        push_book_updates_to_eval(result);
    }
}

void GameSession::handle_cancel(const NetCancel& ev) {
    if (phase_ != SessionPhase::RoundActive) {
        emit_error(ev.slot, "ROUND_NOT_ACTIVE", "round is not active");
        return;
    }
    // Try cancelling on each instrument until one succeeds. ExchangeSession
    // cancel_order requires a specific instrument_id, so we probe all active
    // books — same logic as the old per-book books_[si].cancel loop.
    bool handled = false;
    for (auto s : engine::kAllSuits) {
        const int si = engine::suit_index(s);
        if (!active_suits_[si]) continue;
        auto result = exchange_.cancel_order(
            ev.order_id,
            static_cast<exchange::instrument_id_t>(si),
            ev.slot);
        // Detect OrderNotFound to continue probing next instrument.
        bool not_found = false;
        for (const auto& fev : result.feedback) {
            if (const auto* rej = std::get_if<exchange::OrderRejected>(&fev)) {
                if (rej->code == exchange::OrderRejected::Code::OrderNotFound) {
                    not_found = true; break;
                }
            }
        }
        if (not_found) continue;
        handled = true;
        if (dispatch_result(ev.slot, result) && wipe_on_trade_) apply_global_wipe();
        break;
    }
    if (!handled)
        emit_error(ev.slot, "ORDER_NOT_FOUND", "order " + std::to_string(ev.order_id) + " not found");
}

void GameSession::handle_start_game() {
    if (phase_ != SessionPhase::Lobby) {
        server_log_.warn("start_game", "ignored — not in Lobby phase");
        return;
    }
    server_log_.info("start_game", "beginning countdown session=" + session_id_);
    begin_countdown();
}

void GameSession::handle_owner_start_round() {
    if (phase_ != SessionPhase::InterRound) return;
    server_log_.info("owner_start_round", "owner triggered early round start");
    begin_round();
}

void GameSession::handle_owner_end_game() {
    if (phase_ != SessionPhase::InterRound) return;
    server_log_.info("owner_end_game", "owner force-ended the game");
    end_game(false);
}

void GameSession::handle_permanent_leave(int32_t slot) {
    if (slot < 0 || slot >= static_cast<int32_t>(slots_.size())) return;
    auto& s = slots_[slot];
    if (!s.active) return;  // already marked inactive (double-fire guard)

    s.active    = false;
    s.connected = false;
    active_player_count_--;

    const bool was_real_player = (s.player_id >= 0);
    if (was_real_player) real_player_count_--;

    server_log_.info("permanent_leave",
        "slot=" + std::to_string(slot) + " player=" + s.username +
        " active_remaining=" + std::to_string(active_player_count_) +
        " real_remaining=" + std::to_string(real_player_count_));

    if (phase_ == SessionPhase::RoundActive || phase_ == SessionPhase::InterRound)
        emit_broadcast(serialise::game_player_left_payload(slot, s.username));

    // No real players left — end immediately regardless of phase.
    if (real_player_count_ == 0) {
        end_game(true);
        return;
    }

    // Signal WsServer to consider spawning a replacement bot.
    // WsServer decides whether to act based on the lobby's spawn_bots_on_leave policy.
    if (was_real_player && phase_ == SessionPhase::RoundActive && game_state_) {
        const auto& hand = game_state_->hand(slot);
        const float remaining = std::max(0.0f, std::chrono::duration<float>(
            round_deadline_ - std::chrono::steady_clock::now()).count());
        outbound_.enqueue(GameSpawnBot{
            slot,
            {hand.suit_counts[0], hand.suit_counts[1],
             hand.suit_counts[2], hand.suit_counts[3]},
            s.balance,
            remaining
        });
    }

    bool any_connected = false;
    for (const auto& sl : slots_) {
        if (sl.active && sl.connected) { any_connected = true; break; }
    }
    if (!any_connected && !all_disconnected_) {
        all_disconnected_       = true;
        all_disconnected_since_ = std::chrono::steady_clock::now();
    }

    check_end_condition();
}


void GameSession::begin_countdown() {
    phase_             = SessionPhase::Countdown;
    countdown_deadline_ = std::chrono::steady_clock::now() +
                          std::chrono::seconds(cfg_.game.countdown_seconds);

    const std::string starts_at = steady_to_iso(countdown_deadline_);
    emit_broadcast(nlohmann::json{
        {"type",         "round_starting"},
        {"starts_at",    starts_at},
        {"player_count", static_cast<int>(slots_.size())},
    }.dump());
    server_log_.info("countdown", "starts_at=" + starts_at);
}

void GameSession::begin_round() {
    phase_        = SessionPhase::RoundActive;
    all_disconnected_ = false;
    round_number_++;

    // If no WS players are connected at round start, start the all-disconnected
    // grace timer immediately so the session tears down within kReconnectGrace
    // rather than running for the full round_duration_seconds.
    bool any_connected = false;
    for (const auto& sl : slots_) {
        if (sl.active && sl.connected) { any_connected = true; break; }
    }
    if (!any_connected) {
        all_disconnected_       = true;
        all_disconnected_since_ = std::chrono::steady_clock::now();
    }

    // Signal WsServer to admit queued players before dealing hands.
    outbound_.enqueue(GameRoundStarted{});

    // Reset sequence counter so seq numbers are round-relative for feed consumers.
    exchange_.reset_seq();

    reset_delta_table();

    std::fill(funded_this_round_.begin(), funded_this_round_.end(), false);

    std::uniform_int_distribution<int> deck_pick(0, static_cast<int>(kDecks.size()) - 1);
    current_deck_ = &kDecks[deck_pick(rng_)];

    engine::GameState::Config gs_cfg{
        static_cast<int>(slots_.size()),
        cfg_.game.total_cards,
        current_deck_->distribution,
        current_deck_->goal_suit,
    };
    game_state_ = std::make_unique<engine::GameState>(gs_cfg);
    auto deal   = game_state_->deal(rng_);

    current_buy_in_ = cfg_.scoring.pot_size / active_player_count_;

    try {
        auto conn = db_pool_.acquire();
        pqxx::work txn(conn.get());
        current_round_id_ = SessionRepo{}.create_round(
            txn, session_id_, round_number_, current_goal_suit_str(), cfg_.scoring.pot_size);
        txn.commit();
    } catch (const std::exception& ex) {
        server_log_.warn("db", "create_round failed: " + std::string(ex.what()));
    }

    collect_buy_ins(current_buy_in_);

    round_deadline_ = std::chrono::steady_clock::now() +
                      std::chrono::seconds(cfg_.game.round_duration_seconds);
    const std::string round_end_at = steady_to_iso(round_deadline_);

    deal_and_send_round_start(deal, round_end_at);
    emit_book_state_snapshot();
    push_round_start_to_eval();

    server_log_.info("round",
        "round=" + std::to_string(round_number_) +
        " goal=" + current_goal_suit_str() +
        " round_end_at=" + round_end_at);
}

void GameSession::collect_buy_ins(int buy_in) {
    for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
        if (!slots_[i].active || funded_this_round_[i]) continue;
        slots_[i].balance    -= buy_in;
        funded_this_round_[i] = true;
    }
}

void GameSession::deal_and_send_round_start(
        const engine::DealResult& deal, const std::string& round_end_at) {
    std::vector<std::string> usernames;
    std::vector<int>         all_hand_totals;
    std::vector<int>         all_balances;
    usernames.reserve(slots_.size());
    all_hand_totals.reserve(slots_.size());
    all_balances.reserve(slots_.size());
    for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
        usernames.push_back(slots_[i].username);
        const auto& sc = deal.hands[i].suit_counts;
        all_hand_totals.push_back(sc[0] + sc[1] + sc[2] + sc[3]);
        all_balances.push_back(slots_[i].balance);
    }
    for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
        if (!slots_[i].connected) continue;
        emit_targeted(i, serialise::round_start_payload(
            i, deal.hands[i], round_end_at, slots_[i].balance,
            usernames, all_hand_totals, all_balances, game_mode_));
    }
}

void GameSession::push_round_start_to_eval() {
    recent_trades_.clear();
    round_start_time_ = std::chrono::steady_clock::now();
    eval_runner_->push_round_start(make_eval_snapshot());
    engine_log_.info("eval", "[eval] round_start pushed round=" + std::to_string(round_number_));
}

void GameSession::end_round() {
    apply_global_wipe();

    std::vector<engine::PlayerHand> hands;
    std::vector<bool>               disconnected;
    for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
        hands.push_back(game_state_->hand(i));
        disconnected.push_back(!slots_[i].connected);
    }

    const int goal_si = engine::suit_index(game_state_->goal_suit());
    int total_goal_cards = 0;
    for (const auto& h : hands) total_goal_cards += h.suit_counts[goal_si];
    const int bonus_pool = cfg_.scoring.pot_size - total_goal_cards * cfg_.scoring.points_per_card;
    const engine::ScoringConfig sc{current_buy_in_, cfg_.scoring.points_per_card, bonus_pool};
    const auto result = engine::score_round(
        hands, game_state_->goal_suit(), disconnected, sc);

    engine_log_.info("score_round",
        "round=" + std::to_string(round_number_) + " goal=" + current_goal_suit_str());

    for (int i = 0; i < static_cast<int>(slots_.size()); ++i)
        slots_[i].balance += result.player_results[i].payout;

    std::vector<WirePlayerResult> wire_results;
    for (const auto& pr : result.player_results) {
        wire_results.push_back({
            pr.player_slot,
            pr.goal_cards_held,
            pr.payout,
            slots_[pr.player_slot].balance,
            pr.disconnected,
        });
    }

    try {
        auto conn = db_pool_.acquire();
        pqxx::work txn(conn.get());
        SessionRepo{}.end_round(txn, current_round_id_);
        txn.commit();
    } catch (const std::exception& ex) {
        server_log_.warn("db", "end_round failed: " + std::string(ex.what()));
    }

    round_history_.push_back({round_number_, current_goal_suit_str(), wire_results});

    eval_runner_->push_round_end(make_eval_snapshot());
    engine_log_.info("eval", "[eval] round_end pushed round=" + std::to_string(round_number_));

    begin_inter_round(wire_results, current_goal_suit_str());
}

void GameSession::begin_inter_round(
        const std::vector<WirePlayerResult>& results,
        const std::string& goal_suit) {
    phase_ = SessionPhase::InterRound;

    inter_round_deadline_ = std::chrono::steady_clock::now() +
                            std::chrono::seconds(cfg_.game.inter_round_seconds);
    const std::string next_round_at = steady_to_iso(inter_round_deadline_);

    emit_broadcast(serialise::inter_round_payload(
        round_number_, goal_suit, results, next_round_at));

    server_log_.info("inter_round",
        "round=" + std::to_string(round_number_) + " next_round_at=" + next_round_at);

    check_end_condition();
}


void GameSession::end_game(bool forced) {
    if (phase_ == SessionPhase::Ended) return;
    phase_ = SessionPhase::Ended;

    server_log_.info("game_end",
        "forced=" + std::string(forced ? "true" : "false") +
        " rounds=" + std::to_string(round_number_));

    const int starting = cfg_.scoring.starting_balance;
    std::vector<WireFinalStanding> standings;
    for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
        standings.push_back({
            i, slots_[i].username,
            slots_[i].balance,
            slots_[i].balance - starting,
        });
    }

    std::vector<WireRoundSummary> rounds;
    for (const auto& rh : round_history_)
        rounds.push_back({rh.round_number, rh.goal_suit, rh.results});

    emit_broadcast(serialise::game_ended_payload(rounds, standings));
    persist_game_result();
}

void GameSession::check_end_condition() {
    if (phase_ == SessionPhase::Ended || phase_ != SessionPhase::InterRound) return;
    // min_players governs game start; mid-game we only end if no real players remain.
    if (real_player_count_ == 0) {
        server_log_.info("check_end", "no real players remain — ending game");
        end_game(true);
    }
}


std::string GameSession::current_goal_suit_str() const {
    return current_deck_ ? std::string(engine::suit_name(current_deck_->goal_suit)) : "";
}

// Converts an exchange instrument_id (0–3) back to the suit label string used
// in wire protocol messages. instrument_id == suit_index() so kAllSuits[id] gives
// the correct Suit. Called only from dispatch_result and related emit helpers.
static std::string instrument_suit_label(exchange::instrument_id_t id) {
    return std::string(engine::suit_name(engine::kAllSuits[id]));
}


bool GameSession::dispatch_result(int32_t slot, const exchange::ExchangeResult& result) {
    // Detect trade first so BookUpdated suppression below is correct.
    // After a trade, apply_global_wipe() immediately follows; broadcasting the
    // pre-wipe book state would confuse clients, so BookUpdated is skipped.
    bool had_trade = false;
    for (const auto& mev : result.market) {
        if (std::holds_alternative<exchange::OrderExecuted>(mev)) { had_trade = true; break; }
    }

    int64_t     new_order_id   = -1;
    std::string new_order_suit;

    // Operational feedback → targeted acks/errors and broadcast book updates.
    for (const auto& fev : result.feedback) {
        if (const auto* ack = std::get_if<exchange::OrderAck>(&fev)) {
            new_order_id   = ack->order_id;
            new_order_suit = instrument_suit_label(ack->instrument_id);
            emit_targeted(slot, nlohmann::json{
                {"type",     "order_ack"},
                {"order_id", ack->order_id},
                {"suit",     new_order_suit},
                {"side",     serialise::side(ack->side)},
                {"price",    ack->price},
                {"qty",      ack->qty},
            }.dump());
            if (!wipe_on_trade_) {
                order_qty_map_[ack->order_id] = ack->qty;
                order_orig_qty_map_[ack->order_id] = ack->qty;
            }
        }
        else if (const auto* upd = std::get_if<exchange::BookUpdated>(&fev)) {
            if (!had_trade) {
                const auto  iid  = static_cast<exchange::instrument_id_t>(upd->instrument_id);
                const auto  suit = engine::kAllSuits[upd->instrument_id];
                const exchange::seq_t seq  = exchange_.current_seq();
                outbound_.enqueue(GameBookUpdate{
                    serialise::book_update_payload(
                        instrument_suit_label(upd->instrument_id),
                        upd->best_bid, upd->best_ask, seq,
                        upd->best_bid_slot, upd->best_ask_slot),
                    suit,
                    exchange_.bids_depth(iid, cfg_.market_data.mbp_depth),
                    exchange_.asks_depth(iid, cfg_.market_data.mbp_depth),
                    seq});
            }
        }
        else if (const auto* rej = std::get_if<exchange::OrderRejected>(&fev)) {
            emit_error(slot,
                serialise::error_code_str(to_ws_error_code(rej->code)),
                rej->message);
        }
        else if (const auto* cack = std::get_if<exchange::CancelAck>(&fev)) {
            order_qty_map_.erase(cack->order_id);
            order_orig_qty_map_.erase(cack->order_id);
            emit_targeted(slot, nlohmann::json{
                {"type",     "order_cancel_ack"},
                {"order_id", cack->order_id},
            }.dump());
        }
    }

    for (const auto& mev : result.market) {
        if (const auto* added = std::get_if<exchange::OrderAdded>(&mev)) {
            const engine::Suit suit = engine::kAllSuits[added->instrument_id];
            outbound_.enqueue(GameMboEvent{wire::order_added(
                added->order_id, suit, added->side, added->price, added->seq)});
        } else if (const auto* exec = std::get_if<exchange::OrderExecuted>(&mev)) {
            const std::string suit = instrument_suit_label(exec->instrument_id);
            engine_log_.info("trade",
                "suit=" + suit + " price=" + std::to_string(exec->price));

            for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
                if (!slots_[i].connected) continue;
                nlohmann::json your_side = nullptr;
                if (i == exec->buyer_slot)  your_side = "buy";
                if (i == exec->seller_slot) your_side = "sell";
                emit_targeted(i, nlohmann::json{
                    {"type",             "trade"},
                    {"suit",             suit},
                    {"price",            exec->price},
                    {"aggressor_side",   serialise::side(exec->aggressor_side)},
                    {"your_side",        your_side},
                    {"buyer_slot",       exec->buyer_slot},
                    {"seller_slot",      exec->seller_slot},
                    {"qty_filled",       exec->qty_filled},
                    {"qty_ordered",      exec->qty_ordered},
                    {"passive_order_id", exec->order_id},
                }.dump());
            }
            emit_spectator_broadcast(nlohmann::json{
                {"type",             "trade"},
                {"suit",             suit},
                {"price",            exec->price},
                {"aggressor_side",   serialise::side(exec->aggressor_side)},
                {"your_side",        nullptr},
                {"buyer_slot",       exec->buyer_slot},
                {"seller_slot",      exec->seller_slot},
                {"qty_filled",       exec->qty_filled},
                {"qty_ordered",      exec->qty_ordered},
                {"passive_order_id", exec->order_id},
            }.dump());
            outbound_.enqueue(GameMboEvent{wire::order_executed(
                exec->order_id,
                engine::kAllSuits[exec->instrument_id],
                exec->price,
                exec->aggressor_side,
                exec->buyer_slot,
                exec->seller_slot,
                exec->seq)});

            if (!wipe_on_trade_) {
                const int64_t passive_id = exec->order_id;
                const int passive_slot = (exec->aggressor_side == engine::Side::Buy)
                                         ? exec->seller_slot : exec->buyer_slot;
                auto it = order_qty_map_.find(passive_id);
                if (it != order_qty_map_.end()) {
                    it->second -= exec->qty_filled;
                    if (slots_[passive_slot].connected) {
                        emit_targeted(passive_slot, nlohmann::json{
                            {"type",          "order_partially_filled"},
                            {"order_id",      passive_id},
                            {"suit",          suit},
                            {"qty_remaining", it->second},
                        }.dump());
                    }
                    if (it->second <= 0) {
                        order_qty_map_.erase(it);
                    }
                }
                if (new_order_id >= 0 && new_order_id != passive_id) {
                    auto ait = order_qty_map_.find(new_order_id);
                    if (ait != order_qty_map_.end()) ait->second -= exec->qty_filled;
                }
            }
        } else if (const auto* cxl = std::get_if<exchange::OrderCancelled>(&mev)) {
            outbound_.enqueue(GameMboEvent{wire::order_cancelled(
                cxl->order_id,
                engine::kAllSuits[cxl->instrument_id],
                cxl->seq)});
        }
    }

    // Emit order_partially_filled to the aggressor if their order still rests
    // with remaining qty after all fills in this result.
    if (!wipe_on_trade_ && new_order_id >= 0 && had_trade && slots_[slot].connected) {
        auto ait = order_qty_map_.find(new_order_id);
        if (ait != order_qty_map_.end() && ait->second > 0) {
            emit_targeted(slot, nlohmann::json{
                {"type",          "order_partially_filled"},
                {"order_id",      new_order_id},
                {"suit",          new_order_suit},
                {"qty_remaining", ait->second},
            }.dump());
        } else if (ait != order_qty_map_.end() && ait->second <= 0) {
            order_qty_map_.erase(ait);
        }
    }

    return had_trade;
}

void GameSession::push_book_updates_to_eval(const exchange::ExchangeResult& result) {
    for (const auto& fev : result.feedback) {
        if (const auto* upd = std::get_if<exchange::BookUpdated>(&fev)) {
            const auto suit_opt = engine::suit_from_string(instrument_suit_label(upd->instrument_id));
            if (!suit_opt) continue;
            eval::EvalBookUpdate bu;
            bu.suit          = *suit_opt;
            bu.best_bid      = upd->best_bid;
            bu.best_ask      = upd->best_ask;
            bu.best_bid_slot = upd->best_bid_slot;
            bu.best_ask_slot = upd->best_ask_slot;
            eval_runner_->push_book_update(bu);
        }
    }
}

void GameSession::apply_post_trade_state(const exchange::ExchangeResult& result) {
    apply_card_transfers(result);
    apply_trade_settlements(result);

    const int64_t elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - round_start_time_).count();
    for (const auto& mev : result.market) {
        if (const auto* exec = std::get_if<exchange::OrderExecuted>(&mev)) {
            apply_trade_delta(exec->buyer_slot, exec->seller_slot,
                              static_cast<int>(exec->instrument_id));
            const std::string suit = instrument_suit_label(exec->instrument_id);
            const auto suit_opt    = engine::suit_from_string(suit);
            if (!suit_opt) continue;
            recent_trades_.push_back(
                {exec->buyer_slot, exec->seller_slot, exec->price, *suit_opt, elapsed_ms});
            eval_runner_->push_trade(eval::EvalTradeEvent{
                exec->buyer_slot, exec->seller_slot, exec->price, *suit_opt, elapsed_ms});
            engine_log_.info("eval",
                "[eval] trade pushed suit=" + suit + " price=" + std::to_string(exec->price));
        }
    }
    broadcast_delta_update();

    for (const auto& fev : result.feedback) {
        if (const auto* upd = std::get_if<exchange::BookUpdated>(&fev)) {
            const auto suit_opt =
                engine::suit_from_string(instrument_suit_label(upd->instrument_id));
            if (!suit_opt) continue;
            eval::EvalBookUpdate bu;
            bu.suit          = *suit_opt;
            bu.best_bid      = upd->best_bid;
            bu.best_ask      = upd->best_ask;
            bu.best_bid_slot = upd->best_bid_slot;
            bu.best_ask_slot = upd->best_ask_slot;
            eval_runner_->push_book_update(bu);
        }
    }
}

void GameSession::apply_trade_delta(int buyer_slot, int seller_slot, int suit_idx) {
    int n = static_cast<int>(delta_table_.size());
    if (buyer_slot  >= 0 && buyer_slot  < n) delta_table_[buyer_slot][suit_idx]++;
    if (seller_slot >= 0 && seller_slot < n) delta_table_[seller_slot][suit_idx]--;
}

void GameSession::reset_delta_table() {
    for (auto& row : delta_table_) row.fill(0);
}

nlohmann::json GameSession::serialize_delta_table() const {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& row : delta_table_)
        out.push_back(nlohmann::json::array({row[0], row[1], row[2], row[3]}));
    return out;
}

void GameSession::broadcast_delta_update() {
    engine_log_.info("delta_update", "broadcast");
    emit_broadcast(nlohmann::json{
        {"type",   "delta_update"},
        {"deltas", serialize_delta_table()},
    }.dump());
}

void GameSession::apply_global_wipe() {
    engine_log_.info("global_wipe", "wiping all books");
    order_qty_map_.clear();
    order_orig_qty_map_.clear();
    for (const auto& upd : exchange_.wipe()) {
        if (!active_suits_[upd.instrument_id]) continue;
        const auto  iid  = static_cast<exchange::instrument_id_t>(upd.instrument_id);
        const auto  suit = engine::kAllSuits[upd.instrument_id];
        const exchange::seq_t seq  = exchange_.current_seq();
        outbound_.enqueue(GameBookUpdate{
            serialise::book_update_payload(
                instrument_suit_label(upd.instrument_id),
                upd.best_bid, upd.best_ask, seq),
            suit,
            exchange_.bids_depth(iid, cfg_.market_data.mbp_depth),
            exchange_.asks_depth(iid, cfg_.market_data.mbp_depth),
            seq});
    }
}

void GameSession::emit_book_state_snapshot() {
    nlohmann::json suits = nlohmann::json::array();
    for (int si = 0; si < 4; ++si) {
        if (!active_suits_[si]) continue;
        const auto iid = static_cast<exchange::instrument_id_t>(si);
        const auto best_bid = exchange_.best_bid(iid);
        const auto best_ask = exchange_.best_ask(iid);
        const auto bid_slot = exchange_.best_bid_slot(iid);
        const auto ask_slot = exchange_.best_ask_slot(iid);
        suits.push_back({
            {"suit",          instrument_suit_label(si)},
            {"best_bid",      best_bid ? nlohmann::json(*best_bid) : nlohmann::json(nullptr)},
            {"best_ask",      best_ask ? nlohmann::json(*best_ask) : nlohmann::json(nullptr)},
            {"best_bid_slot", bid_slot ? nlohmann::json(*bid_slot) : nlohmann::json(nullptr)},
            {"best_ask_slot", ask_slot ? nlohmann::json(*ask_slot) : nlohmann::json(nullptr)},
        });
    }
    emit_broadcast(nlohmann::json{
        {"type",  "book_state_snapshot"},
        {"suits", suits},
    }.dump());
}

void GameSession::apply_card_transfers(const exchange::ExchangeResult& result) {
    if (!game_state_) return;
    bool had_trade = false;
    for (const auto& mev : result.market) {
        if (const auto* exec = std::get_if<exchange::OrderExecuted>(&mev)) {
            const auto suit_opt =
                engine::suit_from_string(instrument_suit_label(exec->instrument_id));
            if (!suit_opt) continue;
            game_state_->transfer_card(exec->seller_slot, exec->buyer_slot, *suit_opt);
            had_trade = true;
        }
    }
    if (had_trade) {
        nlohmann::json totals = nlohmann::json::array();
        for (int i = 0; i < game_state_->player_count(); ++i) {
            const auto& sc = game_state_->hand(i).suit_counts;
            totals.push_back(sc[0] + sc[1] + sc[2] + sc[3]);
        }
        emit_broadcast(nlohmann::json{
            {"type",   "hand_totals"},
            {"totals", totals},
        }.dump());
    }
}

void GameSession::apply_trade_settlements(const exchange::ExchangeResult& result) {
    bool had_trade = false;
    for (const auto& mev : result.market) {
        if (const auto* exec = std::get_if<exchange::OrderExecuted>(&mev)) {
            slots_[exec->buyer_slot].balance  -= exec->price;
            slots_[exec->seller_slot].balance += exec->price;
            had_trade = true;
        }
    }
    if (had_trade) {
        nlohmann::json balances = nlohmann::json::array();
        for (const auto& s : slots_) balances.push_back(s.balance);
        emit_broadcast(nlohmann::json{
            {"type",     "all_balances"},
            {"balances", balances},
        }.dump());
    }
}


void GameSession::send_spectator_snapshot(int32_t spectator_id) {
    switch (phase_) {
    case SessionPhase::Lobby:
    case SessionPhase::Ended:
        break;

    case SessionPhase::Countdown:
        emit_spectator_targeted(spectator_id, nlohmann::json{
            {"type",         "round_starting"},
            {"starts_at",    steady_to_iso(countdown_deadline_)},
            {"player_count", static_cast<int>(slots_.size())},
        }.dump());
        break;

    case SessionPhase::RoundActive: {
        for (auto s : engine::kAllSuits) {
            const int si = engine::suit_index(s);
            if (!active_suits_[si]) continue;
            emit_spectator_targeted(spectator_id,
                serialise::book_update_payload(
                    std::string(engine::suit_name(s)),
                    exchange_.best_bid(si),
                    exchange_.best_ask(si),
                    exchange_.current_seq(),
                    exchange_.best_bid_slot(si),
                    exchange_.best_ask_slot(si)));
        }
        if (game_state_) {
            nlohmann::json totals = nlohmann::json::array();
            for (int i = 0; i < game_state_->player_count(); ++i) {
                const auto& sc = game_state_->hand(i).suit_counts;
                totals.push_back(sc[0] + sc[1] + sc[2] + sc[3]);
            }
            emit_spectator_targeted(spectator_id, nlohmann::json{
                {"type",   "hand_totals"},
                {"totals", totals},
            }.dump());
        }
        {
            nlohmann::json balances = nlohmann::json::array();
            for (const auto& s : slots_) balances.push_back(s.balance);
            emit_spectator_targeted(spectator_id, nlohmann::json{
                {"type",     "all_balances"},
                {"balances", balances},
            }.dump());
        }
        {
            emit_spectator_targeted(spectator_id, nlohmann::json{
                {"type",   "delta_update"},
                {"deltas", serialize_delta_table()},
            }.dump());
        }
        {
            nlohmann::json usernames = nlohmann::json::array();
            for (const auto& s : slots_) usernames.push_back(s.username);
            emit_spectator_targeted(spectator_id, nlohmann::json{
                {"type",         "round_starting"},
                {"round_end_at", steady_to_iso(round_deadline_)},
                {"player_count", static_cast<int>(slots_.size())},
                {"usernames",    usernames},
            }.dump());
        }
        break;
    }

    case SessionPhase::InterRound: {
        const auto& last = round_history_.back();
        emit_spectator_targeted(spectator_id,
            serialise::inter_round_payload(
                last.round_number, last.goal_suit, last.results,
                steady_to_iso(inter_round_deadline_)));
        break;
    }
    }
}

void GameSession::handle_spectator_join(const NetSpectatorJoin& ev) {
    spectator_ids_.insert(ev.spectator_id);
    send_spectator_snapshot(ev.spectator_id);
    server_log_.info("spectator_join",
        "spectator_id=" + std::to_string(ev.spectator_id) +
        " name=" + ev.spectator_name +
        " phase=" + std::to_string(static_cast<int>(phase_)));
}

void GameSession::handle_spectator_leave(const NetSpectatorLeave& ev) {
    spectator_ids_.erase(ev.spectator_id);
    server_log_.info("spectator_leave",
        "spectator_id=" + std::to_string(ev.spectator_id));
}


void GameSession::emit_broadcast(const std::string& json) {
    outbound_.enqueue(GameBroadcast{json});
}

void GameSession::emit_targeted(int32_t slot, const std::string& json) {
    outbound_.enqueue(GameTargeted{slot, json});
}

void GameSession::emit_spectator_targeted(int32_t spectator_id, const std::string& json) {
    outbound_.enqueue(GameSpectatorTargeted{spectator_id, json});
}

void GameSession::emit_spectator_broadcast(const std::string& json) {
    outbound_.enqueue(GameSpectatorBroadcast{json});
}

void GameSession::emit_error(int32_t slot, std::string_view code, std::string_view message) {
    emit_targeted(slot, nlohmann::json{
        {"type",    "error"},
        {"code",    std::string(code)},
        {"message", std::string(message)},
    }.dump());
}

void GameSession::broadcast_waiting_for_start() {
    if (phase_ != SessionPhase::Lobby) return;
    int connected = 0;
    for (const auto& s : slots_) if (s.connected) connected++;
    emit_broadcast(nlohmann::json{
        {"type",      "waiting_for_start"},
        {"connected", connected},
        {"required",  cfg_.lobby.min_players},
    }.dump());
}


void GameSession::persist_game_result() {
    try {
        auto conn = db_pool_.acquire();
        pqxx::work txn(conn.get());
        SessionRepo{}.end_session(txn, session_id_, round_number_);
        txn.commit();
    } catch (const std::exception& ex) {
        server_log_.error("db", "persist_game_result failed: " + std::string(ex.what()));
    }
}

void GameSession::persist_error(const std::string& type, const std::string& msg,
                                const nlohmann::json& ctx) {
    try {
        auto conn = db_pool_.acquire();
        pqxx::work txn(conn.get());
        SessionRepo{}.write_error(txn, session_id_, type, msg, ctx);
        txn.commit();
    } catch (const std::exception& ex) {
        server_log_.error("db", "persist_error failed: " + std::string(ex.what()));
    }
}


std::string GameSession::to_iso_string(std::chrono::system_clock::time_point tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm gmt{};
    gmtime_r(&t, &gmt);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &gmt);
    return buf;
}

std::string GameSession::steady_to_iso(std::chrono::steady_clock::time_point tp) {
    const auto sys_now    = std::chrono::system_clock::now();
    const auto steady_now = std::chrono::steady_clock::now();
    const auto delta      = tp - steady_now;
    return to_iso_string(sys_now +
        std::chrono::duration_cast<std::chrono::system_clock::duration>(delta));
}


engine::GameStateSnapshot GameSession::make_eval_snapshot() const {
    engine::GameStateSnapshot snap;
    snap.round_number    = round_number_;
    snap.round_active    = (phase_ == SessionPhase::RoundActive);
    snap.points_per_card = cfg_.scoring.points_per_card;

    if (phase_ == SessionPhase::RoundActive) {
        const auto remaining = round_deadline_ - std::chrono::steady_clock::now();
        snap.time_remaining_s = std::max(0.0,
            std::chrono::duration<double>(remaining).count());
    }

    if (current_deck_)
        snap.current_deck_index = static_cast<int>(current_deck_ - kDecks.data());

    const int n = std::min(static_cast<int>(slots_.size()), 4);
    snap.num_active_slots = n;
    for (int i = 0; i < n; ++i) {
        snap.player_names[i] = slots_[i].username;
        snap.balances[i]     = slots_[i].balance;
        snap.slot_active[i]  = slots_[i].active;
    }

    if (game_state_) {
        for (int i = 0; i < game_state_->player_count() && i < 4; ++i) {
            const auto& h = game_state_->hand(i);
            for (int s = 0; s < 4; ++s)
                snap.hands[i][s] = h.suit_counts[s];
        }
    }

    for (int i = 0; i < n && i < 4; ++i)
        snap.deltas[i] = delta_table_[i];

    for (auto suit : engine::kAllSuits) {
        const int si = engine::suit_index(suit);
        snap.books[si].best_bid  = exchange_.best_bid(si);
        snap.books[si].best_ask  = exchange_.best_ask(si);
        snap.books[si].best_bid_slot = exchange_.best_bid_slot(si);
        snap.books[si].best_ask_slot = exchange_.best_ask_slot(si);
    }

    return snap;
}

void GameSession::drain_eval_output() {
    std::vector<eval::EvalOutput> pending;
    {
        std::lock_guard<std::mutex> lk(eval_out_mu_);
        pending.swap(eval_out_pending_);
    }
    for (auto& out : pending)
        outbound_.enqueue(GameEvalOutput{std::move(out)});
}


void GameSession::handle_player_disconnect(int slot_index) {
    // Thread-safe: enqueues onto the SPSC inbound queue; game-loop thread processes.
    inbound_.enqueue(NetReconnectDisconnect{static_cast<int32_t>(slot_index)});
}

void GameSession::handle_player_reattach(int slot_index,
                                          const std::string& reconnect_token,
                                          int64_t reconnect_expires_at_ms,
                                          Encoding encoding) {
    inbound_.enqueue(NetReconnectReattach{
        static_cast<int32_t>(slot_index),
        reconnect_token,
        reconnect_expires_at_ms,
        encoding});
}


void GameSession::handle_reconnect_disconnect(int32_t slot) {
    if (slot < 0 || slot >= static_cast<int32_t>(slots_.size())) return;
    auto& s = slots_[slot];
    if (!s.active) return;
    s.connected = false;

    server_log_.info("session", "slot " + std::to_string(slot) + " disconnected");

    // Cancel open orders so remaining players see an accurate book immediately.
    cancel_orders_for_slot(slot);

    if (phase_ == SessionPhase::RoundActive) {
        // Start reconnect window; check_reconnect_expirations() fires expiry logic.
        reconnect_deadlines_[slot] = std::chrono::steady_clock::now() +
            std::chrono::seconds(cfg_.reconnect.reconnect_window_seconds);
    }
    // Outside RoundActive, no timer — the slot stays disconnected until the
    // next round when admit_from_queue may fill it, or the session ends.

    if (phase_ == SessionPhase::RoundActive || phase_ == SessionPhase::InterRound)
        emit_broadcast(serialise::game_player_left_payload(slot, s.username));

    bool any_connected = false;
    for (const auto& sl : slots_) {
        if (sl.active && sl.connected) { any_connected = true; break; }
    }
    if (!any_connected && !all_disconnected_) {
        all_disconnected_       = true;
        all_disconnected_since_ = std::chrono::steady_clock::now();
    }
}

void GameSession::handle_reconnect_reattach(int32_t slot,
                                             const std::string& token,
                                             int64_t expires_at_ms,
                                             Encoding encoding) {
    if (slot < 0 || slot >= static_cast<int32_t>(slots_.size())) return;
    auto& s = slots_[slot];

    reconnect_deadlines_[slot].reset();
    s.connected       = true;
    s.encoding        = encoding;
    all_disconnected_ = false;

    server_log_.info("session", "slot " + std::to_string(slot) + " reattached");

    if (phase_ == SessionPhase::RoundActive && game_state_) {
        emit_targeted(slot, build_state_snapshot(slot, token, expires_at_ms));
    } else if (phase_ == SessionPhase::Countdown) {
        // Player connecting for first time during the pre-deal countdown: send them
        // the countdown deadline so their client can show the timer. Mirrors
        // handle_connect's Countdown branch, which only fires for bots (NetConnect).
        emit_targeted(slot, nlohmann::json{
            {"type",         "round_starting"},
            {"starts_at",    steady_to_iso(countdown_deadline_)},
            {"player_count", static_cast<int>(slots_.size())},
        }.dump());
    } else if (phase_ == SessionPhase::Lobby) {
        // Player connecting before the countdown starts: broadcast current fill count.
        broadcast_waiting_for_start();
    }
}


void GameSession::check_reconnect_expirations() {
    // Only runs during RoundActive — timers are not started in other phases.
    const auto now = std::chrono::steady_clock::now();
    for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
        if (!reconnect_deadlines_[i]) continue;
        if (now < *reconnect_deadlines_[i]) continue;

        reconnect_deadlines_[i].reset();

        server_log_.info("session",
            "slot " + std::to_string(i) + " expired — spawning bot");

        auto& s = slots_[i];
        s.active    = false;
        s.connected = false;
        active_player_count_--;
        if (s.player_id >= 0) real_player_count_--;

        // Tell WsServer to send reconnect_window_expired to the stale socket.
        outbound_.enqueue(GameReconnectExpired{i});

        if (real_player_count_ == 0) { end_game(true); return; }

        // Signal WsServer to spawn a replacement bot (same path as permanent leave).
        if (game_state_) {
            const auto& hand = game_state_->hand(i);
            const float remaining = std::max(0.0f, std::chrono::duration<float>(
                round_deadline_ - now).count());
            outbound_.enqueue(GameSpawnBot{
                i,
                {hand.suit_counts[0], hand.suit_counts[1],
                 hand.suit_counts[2], hand.suit_counts[3]},
                s.balance,
                remaining
            });
        }
    }
}


std::vector<CancelledOrder> GameSession::cancel_orders_for_slot(int slot_index) {
    std::vector<CancelledOrder> cancelled;
    auto result = exchange_.cancel_player(static_cast<int32_t>(slot_index));

    // Collect CancelAck entries for the return value.
    for (const auto& fev : result.feedback) {
        if (const auto* cack = std::get_if<exchange::CancelAck>(&fev)) {
            cancelled.push_back({static_cast<int>(cack->instrument_id), cack->order_id});
        }
    }

    // Broadcast book state for every instrument that had at least one cancel.
    // Only emit for active suits; deduplicate by instrument_id (one broadcast per suit).
    std::array<bool, 4> emitted{};
    for (const auto& fev : result.feedback) {
        if (const auto* cack = std::get_if<exchange::CancelAck>(&fev)) {
            const int si = static_cast<int>(cack->instrument_id);
            if (!active_suits_[si] || emitted[si]) continue;
            emitted[si] = true;
            const auto  iid  = static_cast<exchange::instrument_id_t>(si);
            const auto  suit = engine::kAllSuits[si];
            const exchange::seq_t seq  = exchange_.current_seq();
            outbound_.enqueue(GameBookUpdate{
                serialise::book_update_payload(
                    instrument_suit_label(cack->instrument_id),
                    exchange_.best_bid(iid), exchange_.best_ask(iid), seq),
                suit,
                exchange_.bids_depth(iid, cfg_.market_data.mbp_depth),
                exchange_.asks_depth(iid, cfg_.market_data.mbp_depth),
                seq});
        }
    }
    return cancelled;
}


std::string GameSession::build_state_snapshot(int slot_index,
                                               const std::string& reconnect_token,
                                               int64_t reconnect_expires_at_ms) const {
    if (!game_state_) {
        return nlohmann::json{{"type", "game_state_snapshot"},
                              {"error", "no active round"}}.dump();
    }

    const auto& hand = game_state_->hand(slot_index);
    const float remaining = std::max(0.0f, std::chrono::duration<float>(
        round_deadline_ - std::chrono::steady_clock::now()).count());

    nlohmann::json hand_json = {
        {"clubs",    hand.suit_counts[0]},
        {"diamonds", hand.suit_counts[1]},
        {"hearts",   hand.suit_counts[2]},
        {"spades",   hand.suit_counts[3]},
    };

    nlohmann::json books_json = nlohmann::json::object();
    for (auto s : engine::kAllSuits) {
        const int si = engine::suit_index(s);
        if (!active_suits_[si]) continue;
        const std::string suit = std::string(engine::suit_name(s));
        nlohmann::json bids_arr = nlohmann::json::array();
        for (const auto& b : exchange_.bids_snapshot(static_cast<exchange::instrument_id_t>(si))) {
            auto qit = order_qty_map_.find(b.order_id);
            auto oqit = order_orig_qty_map_.find(b.order_id);
            bids_arr.push_back({{"order_id", b.order_id}, {"price", b.price},
                                {"player_slot", b.player_slot},
                                {"qty", oqit != order_orig_qty_map_.end() ? oqit->second : 1},
                                {"qty_remaining", qit != order_qty_map_.end() ? qit->second : 1}});
        }
        nlohmann::json asks_arr = nlohmann::json::array();
        for (const auto& a : exchange_.asks_snapshot(static_cast<exchange::instrument_id_t>(si))) {
            auto qit = order_qty_map_.find(a.order_id);
            auto oqit = order_orig_qty_map_.find(a.order_id);
            asks_arr.push_back({{"order_id", a.order_id}, {"price", a.price},
                                {"player_slot", a.player_slot},
                                {"qty", oqit != order_orig_qty_map_.end() ? oqit->second : 1},
                                {"qty_remaining", qit != order_qty_map_.end() ? qit->second : 1}});
        }
        books_json[suit] = {{"bids", bids_arr}, {"asks", asks_arr}};
    }

    nlohmann::json deltas_json = serialize_delta_table();

    nlohmann::json balances_json = nlohmann::json::array();
    for (const auto& sl : slots_) balances_json.push_back(sl.balance);

    nlohmann::json hand_totals_json = nlohmann::json::array();
    for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
        const auto& h = game_state_->hand(i);
        hand_totals_json.push_back(h.suit_counts[0] + h.suit_counts[1] +
                                   h.suit_counts[2] + h.suit_counts[3]);
    }

    const auto all_scores = compute_all_scores();
    nlohmann::json scores_json = nlohmann::json::array();
    for (auto sc : all_scores) scores_json.push_back(sc);

    nlohmann::json roster_json = nlohmann::json::array();
    for (int i = 0; i < static_cast<int>(slots_.size()); ++i)
        roster_json.push_back({{"player_slot", i}, {"username", slots_[i].username}});

    return nlohmann::json{
        {"type",                  "game_state_snapshot"},
        {"player_slot",           slot_index},
        {"hand",                  hand_json},
        {"order_books",           books_json},
        {"deltas",                deltas_json},
        {"round_timer_remaining", remaining},
        {"all_balances",          balances_json},
        {"all_hand_totals",       hand_totals_json},
        {"all_scores",            scores_json},
        {"roster",                roster_json},
        {"reconnect_token",       reconnect_token},
        {"reconnect_expires_at",  reconnect_expires_at_ms},
        {"game_mode",             game_mode_string(game_mode_)},
    }.dump();
}


engine::PlayerHand GameSession::hand_for_slot(int slot_index) const {
    if (!game_state_ || slot_index < 0 || slot_index >= game_state_->player_count())
        return engine::PlayerHand{};
    return game_state_->hand(slot_index);
}

std::vector<int> GameSession::compute_all_scores() const {
    std::vector<int> scores(slots_.size(), 0);
    for (const auto& rh : round_history_) {
        for (const auto& r : rh.results) {
            if (r.player_slot >= 0 && r.player_slot < static_cast<int>(scores.size()))
                scores[r.player_slot] += r.payout;
        }
    }
    return scores;
}

void GameSession::handle_admit_queue(const NetAdmitQueue& ev) {
    for (const auto& entry : ev.entries) {
        const int idx = entry.slot_index;
        if (idx < 0 || idx >= static_cast<int>(slots_.size())) continue;
        auto& s = slots_[idx];
        if (s.active) continue;  // slot still occupied — caller misidentified it

        s.player_id = entry.player_id;
        s.username  = entry.username;
        s.connected = false;  // WsServer wires the socket independently
        s.active    = true;
        reconnect_deadlines_[idx].reset();
        active_player_count_++;
        real_player_count_++;

        server_log_.info("queue",
            "slot " + std::to_string(idx) + " admitted player=" + entry.username);
    }
}

void GameSession::handle_send_feed_snapshot(int32_t slot, FeedTier tier) {
    if (slot < 0 || slot >= static_cast<int32_t>(slots_.size())) return;
    const exchange::seq_t seq = exchange_.current_seq();
    for (auto suit : engine::kAllSuits) {
        const int si = engine::suit_index(suit);
        if (!active_suits_[si]) continue;
        const auto iid = static_cast<exchange::instrument_id_t>(si);
        switch (tier) {
            case FeedTier::MBP1:
                emit_targeted(slot, serialise::book_update_payload(
                    std::string(engine::suit_name(suit)),
                    exchange_.best_bid(si), exchange_.best_ask(si),
                    seq,
                    exchange_.best_bid_slot(si), exchange_.best_ask_slot(si)));
                break;
            case FeedTier::MBPN:
                emit_targeted(slot, wire::book_depth_snapshot(
                    suit,
                    exchange_.bids_depth(iid, cfg_.market_data.mbp_depth),
                    exchange_.asks_depth(iid, cfg_.market_data.mbp_depth),
                    seq));
                break;
            case FeedTier::MBO:
                emit_targeted(slot, wire::order_book_snapshot(
                    suit,
                    exchange_.bids_mbo(iid),
                    exchange_.asks_mbo(iid),
                    seq));
                break;
        }
    }
}

void GameSession::handle_market_data_connect(int32_t md_id) {
    const exchange::seq_t seq = exchange_.current_seq();
    for (auto suit : engine::kAllSuits) {
        const int si = engine::suit_index(suit);
        if (!active_suits_[si]) continue;
        const auto iid = static_cast<exchange::instrument_id_t>(si);
        emit_market_data_targeted(md_id, wire::order_book_snapshot(
            suit, exchange_.bids_mbo(iid), exchange_.asks_mbo(iid), seq));
    }
}

void GameSession::emit_market_data_targeted(int32_t md_id, const std::string& json) {
    outbound_.enqueue(GameMarketDataTargeted{md_id, json});
}

void GameSession::deactivate_bot_slot(int slot_index) {
    if (slot_index < 0 || slot_index >= static_cast<int>(slots_.size())) return;
    auto& s = slots_[slot_index];
    if (!s.active || s.player_id >= 0) return; // only active bot slots
    s.active    = false;
    s.connected = false;
    active_player_count_--;
    server_log_.info("queue",
        "bot slot " + std::to_string(slot_index) + " deactivated for queue admission");
}

} // namespace anjeer::server
