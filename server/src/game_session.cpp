#include "server/game_session.h"
#include "server/game_session_wire.h"
#include "server/session_repo.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <string>
#include <thread>
#include <variant>

namespace anjeer::server {

// ─── Deck table ───────────────────────────────────────────────────────────────
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

// ─── Internal helpers ─────────────────────────────────────────────────────────

static std::array<engine::OrderBook, 4> build_books(const ServerConfig& cfg) {
    auto make = [&](engine::Suit s) {
        return engine::OrderBook{engine::OrderBook::Config{
            cfg.order_book.min_price,
            cfg.order_book.max_price,
            cfg.order_book.nudge_initial_buy_price,
            cfg.order_book.nudge_initial_sell_price,
            std::string(engine::suit_name(s)),
        }};
    };
    return {
        make(engine::Suit::Clubs),
        make(engine::Suit::Diamonds),
        make(engine::Suit::Hearts),
        make(engine::Suit::Spades),
    };
}

// ─── Constructor / Destructor ─────────────────────────────────────────────────

GameSession::GameSession(
    std::string                                       session_id,
    std::string                                       lobby_id,
    std::vector<SlotInfo>                             slots,
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
    , books_(build_books(ctx.cfg))
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
}

std::unordered_map<int,int> GameSession::slot_balances() const {
    std::unordered_map<int,int> result;
    for (int i = 0; i < static_cast<int>(slots_.size()); ++i)
        if (slots_[i].player_id != -1)
            result[i] = slots_[i].balance;
    return result;
}

// ─── Game loop ────────────────────────────────────────────────────────────────

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
                handle_reconnect_reattach(e.slot, e.reconnect_token, e.reconnect_expires_at_ms);
        }, ev);
    }
}

// ─── NetEvent handlers ────────────────────────────────────────────────────────

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
    all_disconnected_ = false;

    server_log_.info("connect",
        "slot=" + std::to_string(ev.slot) + " player=" + ev.username);

    // Send current book state to this slot
    for (auto s : engine::kAllSuits) {
        const int si = engine::suit_index(s);
        if (!active_suits_[si]) continue;
        emit_targeted(ev.slot, serialise::book_update_payload(
            std::string(engine::suit_name(s)),
            books_[si].best_bid(), books_[si].best_ask()));
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
    auto events = books_[engine::suit_index(*suit_opt)].submit(ev.slot, ev.side, ev.price);
    const bool had_trade = dispatch_events(ev.slot, events);
    if (had_trade) {
        apply_post_trade_state(events);
        apply_global_wipe();
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
    auto events = books_[engine::suit_index(*suit_opt)].nudge(ev.side, ev.slot);
    const bool had_trade = dispatch_events(ev.slot, events);
    if (had_trade) {
        apply_post_trade_state(events);
        apply_global_wipe();
    }
}

void GameSession::handle_cancel(const NetCancel& ev) {
    if (phase_ != SessionPhase::RoundActive) {
        emit_error(ev.slot, "ROUND_NOT_ACTIVE", "round is not active");
        return;
    }
    bool handled = false;
    for (auto s : engine::kAllSuits) {
        const int si = engine::suit_index(s);
        if (!active_suits_[si]) continue;
        auto events = books_[si].cancel(ev.order_id, ev.slot);
        if (events.empty()) continue;
        const auto* err = std::get_if<engine::OrderErrorEvent>(&events.front());
        if (err && err->code == engine::OrderErrorEvent::Code::OrderNotFound) continue;
        handled = true;
        if (dispatch_events(ev.slot, events)) apply_global_wipe();
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

// ─── Session / round lifecycle ────────────────────────────────────────────────

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

    // Signal WsServer to admit queued players before dealing hands.
    outbound_.enqueue(GameRoundStarted{});

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
            usernames, all_hand_totals, all_balances));
    }

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

// ─── Round config helper ──────────────────────────────────────────────────────

std::string GameSession::current_goal_suit_str() const {
    return current_deck_ ? std::string(engine::suit_name(current_deck_->goal_suit)) : "";
}

// ─── Engine event pipeline ────────────────────────────────────────────────────

bool GameSession::dispatch_events(int32_t slot, const std::vector<engine::OrderEvent>& events) {
    bool had_trade = false;

    for (const auto& ev : events) {
        if (const auto* ack = std::get_if<engine::OrderAckEvent>(&ev)) {
            emit_targeted(slot, nlohmann::json{
                {"type",     "order_ack"},
                {"order_id", ack->order_id},
                {"suit",     ack->suit},
                {"side",     serialise::side(ack->side)},
                {"price",    ack->price},
            }.dump());
        }
        else if (const auto* t = std::get_if<engine::TradeEvent>(&ev)) {
            engine_log_.info("trade",
                "suit=" + t->suit + " price=" + std::to_string(t->price));

            // AGENT-CTX: buyer_id/seller_id in TradeEvent are slot indices (not
            // DB player IDs). The engine uses them as array subscripts into the
            // slots_ vector. Exposed as buyer_slot/seller_slot on the wire so the
            // frontend can look them up in the roster for the trade feed display.
            const int buyer_slot  = t->buyer_id;
            const int seller_slot = t->seller_id;

            for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
                if (!slots_[i].connected) continue;
                nlohmann::json your_side = nullptr;
                if (i == buyer_slot)  your_side = "buy";
                if (i == seller_slot) your_side = "sell";
                emit_targeted(i, nlohmann::json{
                    {"type",           "trade"},
                    {"suit",           t->suit},
                    {"price",          t->price},
                    {"aggressor_side", serialise::side(t->aggressor_side)},
                    {"your_side",      your_side},
                    {"buyer_slot",     buyer_slot},
                    {"seller_slot",    seller_slot},
                }.dump());
            }
            emit_spectator_broadcast(nlohmann::json{
                {"type",           "trade"},
                {"suit",           t->suit},
                {"price",          t->price},
                {"aggressor_side", serialise::side(t->aggressor_side)},
                {"your_side",      nullptr},
                {"buyer_slot",     buyer_slot},
                {"seller_slot",    seller_slot},
            }.dump());

            had_trade = true;
        }
        else if (const auto* upd = std::get_if<engine::BookUpdateEvent>(&ev)) {
            if (!had_trade) {
                emit_broadcast(serialise::book_update_payload(
                    upd->suit, upd->best_bid, upd->best_ask,
                    upd->best_bid_player_id, upd->best_ask_player_id));
            }
        }
        else if (const auto* cack = std::get_if<engine::OrderCancelAckEvent>(&ev)) {
            emit_targeted(slot, nlohmann::json{
                {"type",     "order_cancel_ack"},
                {"order_id", cack->order_id},
            }.dump());
        }
        else if (const auto* err = std::get_if<engine::OrderErrorEvent>(&ev)) {
            emit_error(slot,
                serialise::error_code_str(to_ws_error_code(err->code)),
                err->message);
        }
    }

    return had_trade;
}

void GameSession::apply_post_trade_state(const std::vector<engine::OrderEvent>& events) {
    apply_card_transfers(events);
    apply_trade_settlements(events);
    for (const auto& ev : events) {
        if (const auto* t = std::get_if<engine::TradeEvent>(&ev)) {
            const auto suit_opt = engine::suit_from_string(t->suit);
            if (suit_opt) apply_trade_delta(t->buyer_id, t->seller_id, engine::suit_index(*suit_opt));
        }
    }
    broadcast_delta_update();
}

void GameSession::apply_trade_delta(int buyer_slot, int seller_slot, int suit_idx) {
    int n = static_cast<int>(delta_table_.size());
    if (buyer_slot  >= 0 && buyer_slot  < n) delta_table_[buyer_slot][suit_idx]++;
    if (seller_slot >= 0 && seller_slot < n) delta_table_[seller_slot][suit_idx]--;
}

void GameSession::reset_delta_table() {
    for (auto& row : delta_table_) row.fill(0);
}

void GameSession::broadcast_delta_update() {
    // AGENT-CTX: Full snapshot sent every trade — clients never accumulate.
    // 4×4 array: outer index = player slot, inner index = suit (clubs/diamonds/hearts/spades).
    nlohmann::json deltas = nlohmann::json::array();
    for (const auto& row : delta_table_) {
        deltas.push_back(nlohmann::json::array({row[0], row[1], row[2], row[3]}));
    }
    engine_log_.info("delta_update", "broadcast");
    emit_broadcast(nlohmann::json{
        {"type",   "delta_update"},
        {"deltas", deltas},
    }.dump());
}

void GameSession::apply_global_wipe() {
    engine_log_.info("global_wipe", "wiping all books");
    for (auto s : engine::kAllSuits) {
        const int si = engine::suit_index(s);
        if (!active_suits_[si]) continue;
        const auto events = books_[si].wipe();
        for (const auto& ev : events) {
            if (const auto* upd = std::get_if<engine::BookUpdateEvent>(&ev)) {
                emit_broadcast(serialise::book_update_payload(
                    upd->suit, upd->best_bid, upd->best_ask));
            }
        }
    }
}

void GameSession::apply_card_transfers(const std::vector<engine::OrderEvent>& events) {
    if (!game_state_) return;
    bool had_trade = false;
    for (const auto& ev : events) {
        if (const auto* t = std::get_if<engine::TradeEvent>(&ev)) {
            const auto suit_opt = engine::suit_from_string(t->suit);
            if (!suit_opt) continue;
            game_state_->transfer_card(t->seller_id, t->buyer_id, *suit_opt);
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

void GameSession::apply_trade_settlements(const std::vector<engine::OrderEvent>& events) {
    bool had_trade = false;
    for (const auto& ev : events) {
        if (const auto* t = std::get_if<engine::TradeEvent>(&ev)) {
            slots_[t->buyer_id].balance  -= t->price;
            slots_[t->seller_id].balance += t->price;
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

// ─── Spectator handlers ───────────────────────────────────────────────────────

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
                    books_[si].best_bid(),
                    books_[si].best_ask(),
                    std::nullopt,
                    std::nullopt));
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
            nlohmann::json deltas = nlohmann::json::array();
            for (const auto& row : delta_table_)
                deltas.push_back(nlohmann::json::array({row[0], row[1], row[2], row[3]}));
            emit_spectator_targeted(spectator_id, nlohmann::json{
                {"type",   "delta_update"},
                {"deltas", deltas},
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

// ─── Outbound helpers ─────────────────────────────────────────────────────────

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

// ─── DB writes ────────────────────────────────────────────────────────────────

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

// ─── ISO timestamp helpers ────────────────────────────────────────────────────

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

// ─── T9: Reconnect / queue public API ────────────────────────────────────────

void GameSession::handle_player_disconnect(int slot_index) {
    // Thread-safe: enqueues onto the SPSC inbound queue; game-loop thread processes.
    inbound_.enqueue(NetReconnectDisconnect{static_cast<int32_t>(slot_index)});
}

void GameSession::handle_player_reattach(int slot_index,
                                          const std::string& reconnect_token,
                                          int64_t reconnect_expires_at_ms) {
    inbound_.enqueue(NetReconnectReattach{
        static_cast<int32_t>(slot_index),
        reconnect_token,
        reconnect_expires_at_ms});
}

// ─── T9: Reconnect internal handlers ─────────────────────────────────────────

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
                                             int64_t expires_at_ms) {
    if (slot < 0 || slot >= static_cast<int32_t>(slots_.size())) return;
    auto& s = slots_[slot];

    reconnect_deadlines_[slot].reset();
    s.connected       = true;
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

// ─── T9: check_reconnect_expirations ─────────────────────────────────────────

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

        if (real_player_count_ == 0) { end_game(true); return; }
    }
}

// ─── T9: cancel_orders_for_slot ──────────────────────────────────────────────

std::vector<CancelledOrder> GameSession::cancel_orders_for_slot(int slot_index) {
    std::vector<CancelledOrder> cancelled;
    for (auto s : engine::kAllSuits) {
        const int si = engine::suit_index(s);
        if (!active_suits_[si]) continue;
        auto events = books_[si].cancel_player(static_cast<int32_t>(slot_index));
        bool had_cancel = false;
        for (const auto& ev : events) {
            if (const auto* cack = std::get_if<engine::OrderCancelAckEvent>(&ev)) {
                cancelled.push_back({si, cack->order_id});
                had_cancel = true;
            }
        }
        // Broadcast final book state once per suit (not once per cancelled order).
        if (had_cancel) {
            emit_broadcast(serialise::book_update_payload(
                std::string(engine::suit_name(s)),
                books_[si].best_bid(), books_[si].best_ask()));
        }
    }
    return cancelled;
}

// ─── T9: build_state_snapshot ────────────────────────────────────────────────

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
        for (const auto& b : books_[si].bids_snapshot())
            bids_arr.push_back({{"order_id", b.order_id}, {"price", b.price},
                                {"player_slot", b.player_slot}});
        nlohmann::json asks_arr = nlohmann::json::array();
        for (const auto& a : books_[si].asks_snapshot())
            asks_arr.push_back({{"order_id", a.order_id}, {"price", a.price},
                                {"player_slot", a.player_slot}});
        books_json[suit] = {{"bids", bids_arr}, {"asks", asks_arr}};
    }

    nlohmann::json deltas_json = nlohmann::json::array();
    for (const auto& row : delta_table_)
        deltas_json.push_back(nlohmann::json::array({row[0], row[1], row[2], row[3]}));

    nlohmann::json balances_json = nlohmann::json::array();
    for (const auto& sl : slots_) balances_json.push_back(sl.balance);

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
        {"all_scores",            scores_json},
        {"roster",                roster_json},
        {"reconnect_token",       reconnect_token},
        {"reconnect_expires_at",  reconnect_expires_at_ms},
    }.dump();
}

// ─── T9: hand_for_slot / compute_all_scores / admit_from_queue ───────────────

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

int GameSession::admit_from_queue(const std::vector<SlotAdmitInfo>& entries) {
    int admitted = 0;
    for (const auto& entry : entries) {
        const int idx = entry.slot_index;
        if (idx < 0 || idx >= static_cast<int>(slots_.size())) continue;
        auto& s = slots_[idx];
        if (s.active) continue;  // slot is still occupied — caller misidentified it

        s.player_id = entry.player_id;
        s.username  = entry.username;
        s.connected = false;  // WsServer wires the socket after this call returns
        s.active    = true;
        reconnect_deadlines_[idx].reset();
        active_player_count_++;
        real_player_count_++;
        ++admitted;

        server_log_.info("queue",
            "slot " + std::to_string(idx) + " admitted player=" + entry.username);
    }
    return admitted;
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
