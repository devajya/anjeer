#include "server/ws_server.h"
#include "server/logger.h"

// AGENT-CTX: engine/ is the only non-server dependency here. The coupling is
// intentional (server owns the books in Slice 2). In Slice 6+, when each lobby
// runs its own thread, these OrderBook instances move into a GameSession object
// and the server communicates with them via a command queue + Loop::defer().
// See cpp_performance_rules.md "separate network thread from game loop thread".
#include "engine/engine.h"

#include <App.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <random>
#include <array>
#include <set>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace anjeer::server {

// AGENT-CTX: PerSocketData is uWS per-connection user data. player_slot is the
// 0-indexed slot assigned on connect; -1 = unassigned sentinel.
// Slice 5 adds jwt_token or similar auth credential here.
struct PerSocketData {
    int32_t player_slot = -1;  // lobby seam: Slice 6
};

using WsHandle = uWS::WebSocket<false, true, PerSocketData>*;

// ═══════════════════════════════════════════════════════════════════════════
// Wire-protocol error codes
// ═══════════════════════════════════════════════════════════════════════════

// AGENT-CTX: WsErrorCode is the server's complete wire-protocol error code set.
// Engine codes (PriceOutOfRange, OrderNotFound, NotYourOrder) reach here via
// to_ws_error_code(); server-layer codes (UnknownSuit, MalformedMessage) are
// referenced directly at each call site. This keeps engine::OrderErrorEvent::Code
// free of protocol concepts — the engine never needs to know about suits or JSON.
enum class WsErrorCode {
    PriceOutOfRange,
    OrderNotFound,
    NotYourOrder,
    UnknownSuit,
    MalformedMessage,
    ServerFull,
};

// AGENT-CTX: Maps engine::OrderErrorEvent::Code → WsErrorCode. An exhaustive
// switch here means adding a new engine code causes a compile error until the
// server explicitly handles it — intentional coupling at the boundary.
static WsErrorCode to_ws_error_code(engine::OrderErrorEvent::Code c) noexcept {
    switch (c) {
        case engine::OrderErrorEvent::Code::PriceOutOfRange: return WsErrorCode::PriceOutOfRange;
        case engine::OrderErrorEvent::Code::OrderNotFound:   return WsErrorCode::OrderNotFound;
        case engine::OrderErrorEvent::Code::NotYourOrder:    return WsErrorCode::NotYourOrder;
    }
    return WsErrorCode::PriceOutOfRange;  // unreachable; silences -Wreturn-type
}

// ═══════════════════════════════════════════════════════════════════════════
// namespace serialise — event→JSON and socket-send layer
//
// AGENT-CTX: Pure output — converts data to wire format and pushes bytes to
// sockets. No game-state reads or writes. All functions are either:
//   (a) builders that return a JSON string, or
//   (b) senders that call ws->send() or iterate conns.
// This layer has no dependency on GameSession or the engine beyond the event
// types it receives. Swapping the wire format means editing only this namespace.
// ═══════════════════════════════════════════════════════════════════════════
namespace serialise {

static std::string error_code_str(WsErrorCode c) noexcept {
    switch (c) {
        case WsErrorCode::PriceOutOfRange:  return "PRICE_OUT_OF_RANGE";
        case WsErrorCode::OrderNotFound:    return "ORDER_NOT_FOUND";
        case WsErrorCode::NotYourOrder:     return "NOT_YOUR_ORDER";
        case WsErrorCode::UnknownSuit:      return "UNKNOWN_SUIT";
        case WsErrorCode::MalformedMessage: return "MALFORMED_MESSAGE";
        case WsErrorCode::ServerFull:       return "SERVER_FULL";
    }
    return "UNKNOWN_ERROR";
}

static std::string side(engine::Side s) noexcept {
    return s == engine::Side::Buy ? "buy" : "sell";
}

static std::string opt_price(std::optional<int32_t> p) noexcept {
    return p.has_value() ? std::to_string(*p) : "null";
}

// AGENT-CTX: Shared between the on-connect snapshot and broadcast_book_update
// so the wire format stays in sync regardless of call site.
static std::string book_update_payload(
        const std::string&     suit,
        std::optional<int32_t> best_bid,
        std::optional<int32_t> best_ask) {
    const nlohmann::json bid_val =
        best_bid.has_value() ? nlohmann::json(*best_bid) : nlohmann::json(nullptr);
    const nlohmann::json ask_val =
        best_ask.has_value() ? nlohmann::json(*best_ask) : nlohmann::json(nullptr);
    return nlohmann::json{
        {"type",     "book_update"},
        {"suit",     suit},
        {"best_bid", bid_val},
        {"best_ask", ask_val},
    }.dump();
}

static void error(WsHandle ws,
                  WsErrorCode code,
                  std::string_view msg,
                  Logger& slog) {
    const std::string code_str = error_code_str(code);
    const std::string payload =
        nlohmann::json{{"type","error"},{"code",code_str},{"message",msg}}.dump();
    ws->send(payload, uWS::OpCode::TEXT);
    slog.warn("send",
              "player=" + std::to_string(ws->getUserData()->player_slot) +
              " type=error code=" + code_str +
              " message=" + std::string(msg));
}

// AGENT-CTX: best_bid / best_ask are sent as JSON null when nullopt (no resting
// orders). Clients must handle null on both fields — this happens after a wipe.
static void book_update(
        const std::set<WsHandle>& conns,
        const std::string&        suit,
        std::optional<int32_t>    best_bid,
        std::optional<int32_t>    best_ask,
        Logger&                   slog) {
    const std::string payload = book_update_payload(suit, best_bid, best_ask);
    for (WsHandle c : conns) c->send(payload, uWS::OpCode::TEXT);
    slog.info("broadcast",
              "type=book_update suit=" + suit +
              " best_bid=" + opt_price(best_bid) +
              " best_ask=" + opt_price(best_ask) +
              " recipients=" + std::to_string(conns.size()));
}

// AGENT-CTX: your_side is null for spectators (anyone who is neither buyer nor
// seller). In Slice 2 with exactly 2 clients, everyone is a participant.
static void trade(
        const std::set<WsHandle>& conns,
        const engine::TradeEvent& t,
        Logger&                   slog) {
    slog.info("broadcast",
              "type=trade suit=" + t.suit +
              " price=" + std::to_string(t.price) +
              " aggressor=" + side(t.aggressor_side) +
              " buyer_id=" + std::to_string(t.buyer_id) +
              " seller_id=" + std::to_string(t.seller_id) +
              " recipients=" + std::to_string(conns.size()));

    for (WsHandle c : conns) {
        const int32_t pid = c->getUserData()->player_slot;
        nlohmann::json your_side = nullptr;
        if (pid == t.buyer_id)  your_side = "buy";
        if (pid == t.seller_id) your_side = "sell";

        const std::string payload = nlohmann::json{
            {"type",           "trade"},
            {"suit",           t.suit},
            {"price",          t.price},
            {"aggressor_side", side(t.aggressor_side)},
            {"your_side",      your_side},
        }.dump();
        c->send(payload, uWS::OpCode::TEXT);
    }
}

} // namespace serialise

// ═══════════════════════════════════════════════════════════════════════════
// GameSession — all mutable game state owned by the event-loop thread
// ═══════════════════════════════════════════════════════════════════════════

// AGENT-CTX: All mutable game state lives here, on the uWS event-loop thread.
// In Slice 6+ this moves into a dedicated game-loop thread with a command queue.
struct GameSession {
    // books is indexed by engine::suit_index(). All 4 are always constructed;
    // active_suits[i] marks whether suit i is in the active_suits config list.
    std::array<engine::OrderBook, 4> books;
    std::array<bool, 4>              active_suits{};

    std::set<WsHandle> connections;

    // AGENT-CTX: Slot-based tracking — lobby seam: moves to LobbyManager in Slice 6.
    std::vector<WsHandle> player_slots;
    int                   connected_count      = 0;
    bool                  countdown_in_progress = false;  // set immediately on begin_countdown
    bool                  round_started         = false;  // set after the deal fires

    // AGENT-CTX: game_state is null until countdown fires.
    std::unique_ptr<engine::GameState> game_state;

    explicit GameSession(std::array<engine::OrderBook, 4> b, std::array<bool, 4> a)
        : books(std::move(b)), active_suits(a) {}
};

// ═══════════════════════════════════════════════════════════════════════════
// dispatch_events — pure output layer
// ═══════════════════════════════════════════════════════════════════════════

// AGENT-CTX: Serialises engine events and sends them to the appropriate sockets.
// No game-state mutation — it does not clear books. Returns had_trade so the
// caller can decide whether to apply the global wipe via apply_global_wipe().
// BookUpdateEvent is suppressed when a trade occurred in the same batch
// because the caller will broadcast null book_updates after the wipe instead.
static bool dispatch_events(
        WsHandle                               ws,
        const std::set<WsHandle>&              conns,
        const std::vector<engine::OrderEvent>& events,
        Logger&                                slog,
        Logger&                                elog) {
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
            slog.info("send",
                      "player=" + std::to_string(ws->getUserData()->player_slot) +
                      " type=order_ack order_id=" + std::to_string(ack->order_id) +
                      " suit=" + ack->suit +
                      " side=" + serialise::side(ack->side) +
                      " price=" + std::to_string(ack->price));
            elog.info("order_ack",
                      "order_id=" + std::to_string(ack->order_id) +
                      " player=" + std::to_string(ws->getUserData()->player_slot) +
                      " suit=" + ack->suit +
                      " side=" + serialise::side(ack->side) +
                      " price=" + std::to_string(ack->price));
        }

        else if (const auto* t = std::get_if<engine::TradeEvent>(&ev)) {
            elog.info("trade",
                      "suit=" + t->suit +
                      " price=" + std::to_string(t->price) +
                      " aggressor=" + serialise::side(t->aggressor_side) +
                      " buyer_id=" + std::to_string(t->buyer_id) +
                      " seller_id=" + std::to_string(t->seller_id));
            serialise::trade(conns, *t, slog);
            had_trade = true;
        }

        else if (const auto* upd = std::get_if<engine::BookUpdateEvent>(&ev)) {
            if (!had_trade) {
                serialise::book_update(conns, upd->suit, upd->best_bid, upd->best_ask, slog);
            } else {
                elog.debug("book_update",
                           "suppressed (trade in same batch) suit=" + upd->suit);
            }
        }

        else if (const auto* cack = std::get_if<engine::OrderCancelAckEvent>(&ev)) {
            const std::string payload = nlohmann::json{
                {"type",     "order_cancel_ack"},
                {"order_id", cack->order_id},
            }.dump();
            ws->send(payload, uWS::OpCode::TEXT);
            slog.info("send",
                      "player=" + std::to_string(ws->getUserData()->player_slot) +
                      " type=order_cancel_ack order_id=" + std::to_string(cack->order_id));
            elog.info("cancel_ack",
                      "order_id=" + std::to_string(cack->order_id) +
                      " player=" + std::to_string(ws->getUserData()->player_slot));
        }

        else if (const auto* err = std::get_if<engine::OrderErrorEvent>(&ev)) {
            elog.warn("engine_error",
                      "code=" + serialise::error_code_str(to_ws_error_code(err->code)) +
                      " message=" + err->message +
                      " player=" + std::to_string(ws->getUserData()->player_slot));
            serialise::error(ws, to_ws_error_code(err->code), err->message, slog);
        }
    }

    return had_trade;
}

// AGENT-CTX: Global wipe mechanic (Slice 2 resolution 1).
// Any executed trade resets ALL suit order books simultaneously.
// Separated from dispatch_events so that the serialise layer (event→JSON/socket)
// and game-mechanic policy (wipe books on trade) are independently testable.
// Called by message handlers when dispatch_events returns had_trade=true.
// Each book owns its own state mutation and event production via wipe(); the server
// only fans the returned BookUpdateEvents out to connected clients.
static void apply_global_wipe(GameSession& session, Logger& slog, Logger& elog) {
    elog.info("global_wipe", "wiping all books after trade");
    for (auto s : engine::kAllSuits) {
        const int si = engine::suit_index(s);
        if (!session.active_suits[si]) continue;
        const auto events = session.books[si].wipe();
        for (const auto& ev : events) {
            if (const auto* upd = std::get_if<engine::BookUpdateEvent>(&ev)) {
                serialise::book_update(session.connections, upd->suit,
                                       upd->best_bid, upd->best_ask, slog);
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// namespace parse — JSON→typed-command layer
//
// AGENT-CTX: Pure parsing — no socket I/O, no game-state access.
// Each function returns std::nullopt on any field error; the caller sends
// the wire error. Keeping I/O out of this layer makes it trivially testable
// and means changing the wire format means editing only this namespace and
// namespace serialise.
// ═══════════════════════════════════════════════════════════════════════════
namespace parse {

struct SubmitOrderFields { std::string suit; std::string side; int32_t price; };
struct NudgeFields        { std::string suit; std::string side; };
struct CancelFields       { int64_t order_id; };

static std::optional<SubmitOrderFields> submit_order(const nlohmann::json& j) {
    try {
        return SubmitOrderFields{
            j.at("suit").get<std::string>(),
            j.at("side").get<std::string>(),
            j.at("price").get<int32_t>(),
        };
    } catch (const nlohmann::json::exception&) { return std::nullopt; }
}

static std::optional<NudgeFields> nudge(const nlohmann::json& j) {
    try {
        return NudgeFields{
            j.at("suit").get<std::string>(),
            j.at("side").get<std::string>(),
        };
    } catch (const nlohmann::json::exception&) { return std::nullopt; }
}

static std::optional<CancelFields> cancel_order(const nlohmann::json& j) {
    try {
        return CancelFields{ j.at("order_id").get<int64_t>() };
    } catch (const nlohmann::json::exception&) { return std::nullopt; }
}

static std::optional<engine::Side> side(const std::string& s) noexcept {
    if (s == "buy")  return engine::Side::Buy;
    if (s == "sell") return engine::Side::Sell;
    return std::nullopt;
}

} // namespace parse

// ─── validate_suit_and_side ──────────────────────────────────────────────
// Parses suit and side from wire strings; sends error and returns nullopt on any
// failure. "known suits" is always included in the UNKNOWN_SUIT log.

struct SuitSideResult {
    engine::Suit suit;
    engine::Side side;
};

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

// ═══════════════════════════════════════════════════════════════════════════
// Message handlers
// ═══════════════════════════════════════════════════════════════════════════

static void log_recv(int32_t player_slot, std::string_view msg, Logger& slog) {
    constexpr std::size_t kMaxRawLog = 256;
    std::string raw(msg.substr(0, kMaxRawLog));
    for (auto& c : raw) if (c == '\n' || c == '\r') c = ' ';
    slog.info("recv",
              "player=" + std::to_string(player_slot) +
              " raw=" + raw +
              (msg.size() > kMaxRawLog ? "…" : ""));
}

static void handle_submit(WsHandle ws, const nlohmann::json& j, int32_t player_slot,
                          GameSession& session, Logger& slog, Logger& elog) {
    const auto fields = parse::submit_order(j);
    if (!fields) {
        slog.error("submit_order",
                   "player=" + std::to_string(player_slot) + " missing required fields");
        serialise::error(ws, WsErrorCode::MalformedMessage,
                         "submit_order requires 'suit', 'side', 'price'", slog);
        return;
    }

    elog.info("submit_order",
              "player=" + std::to_string(player_slot) +
              " suit=" + fields->suit +
              " side=" + fields->side +
              " price=" + std::to_string(fields->price));

    const auto validated = validate_suit_and_side(
        ws, session.active_suits, fields->suit, fields->side, "submit_order", slog, elog);
    if (!validated) return;

    elog.debug("submit_order",
               "calling engine — suit=" + fields->suit +
               " player=" + std::to_string(player_slot) +
               " side=" + fields->side +
               " price=" + std::to_string(fields->price));
    auto events = session.books[engine::suit_index(validated->suit)]
                      .submit(player_slot, validated->side, fields->price);
    elog.info("submit_order",
              "engine returned " + std::to_string(events.size()) + " event(s)");

    if (dispatch_events(ws, session.connections, events, slog, elog))
        apply_global_wipe(session, slog, elog);
}

static void handle_nudge(WsHandle ws, const nlohmann::json& j, int32_t player_slot,
                         GameSession& session, Logger& slog, Logger& elog) {
    const auto fields = parse::nudge(j);
    if (!fields) {
        slog.error("nudge",
                   "player=" + std::to_string(player_slot) + " missing required fields");
        serialise::error(ws, WsErrorCode::MalformedMessage,
                         "nudge requires 'suit' and 'side'", slog);
        return;
    }

    elog.info("nudge",
              "player=" + std::to_string(player_slot) +
              " suit=" + fields->suit +
              " side=" + fields->side);

    const auto validated = validate_suit_and_side(
        ws, session.active_suits, fields->suit, fields->side, "nudge", slog, elog);
    if (!validated) return;

    auto events = session.books[engine::suit_index(validated->suit)]
                      .nudge(validated->side, player_slot);
    elog.info("nudge",
              "engine returned " + std::to_string(events.size()) + " event(s)");

    if (dispatch_events(ws, session.connections, events, slog, elog))
        apply_global_wipe(session, slog, elog);
}

static void handle_cancel(WsHandle ws, const nlohmann::json& j, int32_t player_slot,
                          GameSession& session, Logger& slog, Logger& elog) {
    const auto fields = parse::cancel_order(j);
    if (!fields) {
        slog.error("cancel_order",
                   "player=" + std::to_string(player_slot) + " missing required fields");
        serialise::error(ws, WsErrorCode::MalformedMessage,
                         "cancel_order requires 'order_id' (int)", slog);
        return;
    }

    elog.info("cancel_order",
              "player=" + std::to_string(player_slot) +
              " order_id=" + std::to_string(fields->order_id));

    // AGENT-CTX: Cancel searches ALL books because the wire protocol
    // omits suit from the cancel message.
    bool handled = false;
    for (auto s : engine::kAllSuits) {
        const int si = engine::suit_index(s);
        if (!session.active_suits[si]) continue;
        auto events = session.books[si].cancel(fields->order_id, player_slot);
        if (events.empty()) continue;
        const auto* err = std::get_if<engine::OrderErrorEvent>(&events.front());
        if (err && err->code == engine::OrderErrorEvent::Code::OrderNotFound) continue;

        handled = true;
        if (dispatch_events(ws, session.connections, events, slog, elog))
            apply_global_wipe(session, slog, elog);
        break;
    }

    if (!handled) {
        elog.warn("cancel_order",
                  "order_id=" + std::to_string(fields->order_id) + " not found in any book");
        serialise::error(ws, WsErrorCode::OrderNotFound,
                         "order " + std::to_string(fields->order_id) + " not found", slog);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// WsServer
// ═══════════════════════════════════════════════════════════════════════════

WsServer::WsServer(const ServerConfig& cfg) : cfg_(cfg) {}

void WsServer::run() {
    // AGENT-CTX: Log files live in logs/ relative to the process working directory.
    // `make dev` runs the server from the project root, so logs appear at
    // <project_root>/logs/server_logs.txt etc.  The Logger constructor creates
    // the directory if it does not exist.
    Logger server_log("logs/server_logs.txt");
    Logger engine_log("logs/engine_logs.txt");
    Logger frontend_log("logs/frontend_logs.txt");

    server_log.info("startup", "server starting — config loaded");

    // Build all 4 books with identical price config; each gets its own suit name.
    auto make_book = [&](engine::Suit s) {
        return engine::OrderBook{engine::OrderBook::Config{
            cfg_.order_book.min_price,
            cfg_.order_book.max_price,
            cfg_.order_book.nudge_initial_buy_price,
            cfg_.order_book.nudge_initial_sell_price,
            std::string(engine::suit_name(s)),
        }};
    };
    std::array<engine::OrderBook, 4> books_arr{
        make_book(engine::Suit::Clubs),
        make_book(engine::Suit::Diamonds),
        make_book(engine::Suit::Hearts),
        make_book(engine::Suit::Spades),
    };

    // Validate active_suits from config; fail loud on any unrecognized name.
    std::array<bool, 4> active_arr{};
    for (const auto& suit_str : cfg_.order_book.active_suits) {
        const auto s = engine::suit_from_string(suit_str);
        if (!s) throw std::runtime_error(
            "active_suits contains unrecognized suit: '" + suit_str + "'");
        active_arr[engine::suit_index(*s)] = true;
        server_log.info("startup", "registered suit: " + suit_str);
    }

    GameSession session(std::move(books_arr), active_arr);
    session.player_slots.resize(cfg_.game.player_count, nullptr);

    // AGENT-CTX: Seeded once per process from OS entropy. Single instance shared
    // across all deals in this process lifetime (safe — all engine calls are on
    // the event-loop thread, no concurrent access).
    std::mt19937 rng{std::random_device{}()};

    std::atomic<bool> running{true};
    uWS::App   app;
    uWS::Loop* loop = uWS::Loop::get();

    // AGENT-CTX: Stored (not detached) so run() can join before locals are
    // destroyed — the thread captures session, loggers, rng by reference.
    std::optional<std::thread> countdown_thread;

    // ── begin_countdown ───────────────────────────────────────────────────────
    // Broadcasts round_starting, then spawns a thread that sleeps countdown_seconds
    // and defers the deal + per-player round_start to the event-loop thread.
    auto begin_countdown = [&]() {
        session.countdown_in_progress = true;

        // Compute absolute fire time as ISO 8601 UTC string.
        // AGENT-CTX: Absolute timestamp so API clients can compute remaining time
        // regardless of when they receive the message (one message, client timer).
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
        for (WsHandle ws : session.connections)
            ws->send(payload, uWS::OpCode::TEXT);
        server_log.info("round",
                        "round_starting broadcast starts_at=" + starts_at_str +
                        " player_count=" + std::to_string(cfg_.game.player_count));

        countdown_thread.emplace([&]() {
            std::this_thread::sleep_for(
                std::chrono::seconds(cfg_.game.countdown_seconds));

            loop->defer([&]() {
                // AGENT-CTX: Guard against double-fire and post-shutdown access.
                if (!running || session.round_started) return;
                session.round_started = true;

                engine::GameState::Config gs_cfg{
                    cfg_.game.player_count,
                    cfg_.game.total_cards,
                    cfg_.game.card_distribution,
                };
                session.game_state = std::make_unique<engine::GameState>(gs_cfg);
                auto deal = session.game_state->deal(rng);

                // AGENT-CTX: grep `has_extra_card` to find and disable this log
                // when the EV module supersedes it.
                if (deal.uneven_deal) {
                    for (int p = 0; p < static_cast<int>(deal.hands.size()); ++p) {
                        if (deal.hands[p].has_extra_card) {
                            server_log.warn("round",
                                "UNEVEN_DEAL slot=" + std::to_string(p) +
                                " has informational edge — see future EV module.");
                        }
                    }
                }

                // goal_suit logged but never sent to clients (withheld until round end).
                engine_log.info("deal",
                    "goal_suit=" + std::string(engine::suit_name(deal.goal_suit)) +
                    " totals[clubs="    + std::to_string(deal.suit_totals[engine::suit_index(engine::Suit::Clubs)])    +
                    " diamonds=" + std::to_string(deal.suit_totals[engine::suit_index(engine::Suit::Diamonds)]) +
                    " hearts="   + std::to_string(deal.suit_totals[engine::suit_index(engine::Suit::Hearts)])   +
                    " spades="   + std::to_string(deal.suit_totals[engine::suit_index(engine::Suit::Spades)])   + "]");

                // Send each player only their own hand.
                for (int slot = 0; slot < cfg_.game.player_count; ++slot) {
                    WsHandle ws = session.player_slots[slot];
                    if (!ws) {
                        server_log.warn("round",
                            "slot=" + std::to_string(slot) +
                            " disconnected during countdown — hand not sent");
                        continue;
                    }
                    const auto& h = deal.hands[slot];
                    const std::string hand_payload = nlohmann::json{
                        {"type",        "round_start"},
                        {"player_slot", slot},
                        {"hand", {
                            {"clubs",    h.suit_counts[engine::suit_index(engine::Suit::Clubs)]},
                            {"diamonds", h.suit_counts[engine::suit_index(engine::Suit::Diamonds)]},
                            {"hearts",   h.suit_counts[engine::suit_index(engine::Suit::Hearts)]},
                            {"spades",   h.suit_counts[engine::suit_index(engine::Suit::Spades)]},
                        }},
                    }.dump();
                    ws->send(hand_payload, uWS::OpCode::TEXT);
                    server_log.info("send",
                        "player=" + std::to_string(slot) + " type=round_start" +
                        " clubs="    + std::to_string(h.suit_counts[0]) +
                        " diamonds=" + std::to_string(h.suit_counts[1]) +
                        " hearts="   + std::to_string(h.suit_counts[2]) +
                        " spades="   + std::to_string(h.suit_counts[3]));
                }

                server_log.info("round",
                    "round started — goal_suit=" +
                    std::string(engine::suit_name(deal.goal_suit)) +
                    " (withheld from clients until round end)");
            });
        });
    };

    // ── WebSocket handler ─────────────────────────────────────────────────
    app.ws<PerSocketData>("/ws", {
        .idleTimeout            = static_cast<unsigned short>(cfg_.ping_timeout_ms / 1000),
        .sendPingsAutomatically = true,

        // ── open ──────────────────────────────────────────────────────────
        .open = [&](WsHandle ws) {
            // Find the first empty slot. Reject if all slots are taken.
            int slot = -1;
            for (int i = 0; i < static_cast<int>(session.player_slots.size()); ++i) {
                if (!session.player_slots[i]) { slot = i; break; }
            }
            if (slot == -1) {
                serialise::error(ws, WsErrorCode::ServerFull,
                                 "no player slots available", server_log);
                ws->close();
                server_log.warn("open", "connection rejected — all slots filled");
                return;
            }

            session.player_slots[slot] = ws;
            session.connected_count++;
            ws->getUserData()->player_slot = static_cast<int32_t>(slot);
            session.connections.insert(ws);
            server_log.info("open",
                            "slot=" + std::to_string(slot) +
                            " connected=" + std::to_string(session.connected_count) +
                            "/" + std::to_string(cfg_.game.player_count));

            ws->send(nlohmann::json{{"type","player_hello"},{"player_id",slot}}.dump(),
                     uWS::OpCode::TEXT);
            server_log.info("send",
                            "player=" + std::to_string(slot) +
                            " type=player_hello player_id=" + std::to_string(slot));

            for (auto s : engine::kAllSuits) {
                const int si = engine::suit_index(s);
                if (!session.active_suits[si]) continue;
                const auto& book     = session.books[si];
                const std::string ss = std::string(engine::suit_name(s));
                ws->send(serialise::book_update_payload(ss, book.best_bid(), book.best_ask()),
                         uWS::OpCode::TEXT);
                server_log.info("send",
                                "player=" + std::to_string(slot) +
                                " type=book_update(on-connect) suit=" + ss +
                                " best_bid=" + serialise::opt_price(book.best_bid()) +
                                " best_ask=" + serialise::opt_price(book.best_ask()));
            }

            // Trigger countdown once all slots are filled (exactly once).
            if (!session.countdown_in_progress &&
                session.connected_count == cfg_.game.player_count) {
                begin_countdown();
            }
        },

        // ── message ───────────────────────────────────────────────────────
        .message = [&](WsHandle ws, std::string_view msg, uWS::OpCode op) {
            if (op != uWS::OpCode::TEXT) {
                server_log.warn("recv", "ignored non-TEXT frame");
                return;
            }

            const int32_t player_slot = ws->getUserData()->player_slot;
            log_recv(player_slot, msg, server_log);

            nlohmann::json j;
            try {
                j = nlohmann::json::parse(msg);
            } catch (const nlohmann::json::exception& ex) {
                server_log.error("recv",
                                 "player=" + std::to_string(player_slot) +
                                 " JSON parse failed: " + ex.what());
                serialise::error(ws, WsErrorCode::MalformedMessage, "invalid JSON", server_log);
                return;
            }

            std::string type;
            try {
                type = j.at("type").get<std::string>();
            } catch (const nlohmann::json::exception& ex) {
                server_log.error("recv",
                                 "player=" + std::to_string(player_slot) +
                                 " missing 'type': " + ex.what());
                serialise::error(ws, WsErrorCode::MalformedMessage,
                                 "missing or invalid 'type' field", server_log);
                return;
            }

            if      (type == "submit_order") handle_submit(ws, j, player_slot, session, server_log, engine_log);
            else if (type == "nudge")        handle_nudge (ws, j, player_slot, session, server_log, engine_log);
            else if (type == "cancel_order") handle_cancel(ws, j, player_slot, session, server_log, engine_log);
            else {
                server_log.warn("recv",
                                "player=" + std::to_string(player_slot) +
                                " unknown type: '" + type + "'");
                serialise::error(ws, WsErrorCode::MalformedMessage,
                                 "unknown message type: '" + type + "'", server_log);
            }
        },

        // ── close ─────────────────────────────────────────────────────────
        .close = [&](WsHandle ws, int code, std::string_view /*msg*/) {
            const int32_t slot = ws->getUserData()->player_slot;
            session.connections.erase(ws);
            if (slot >= 0 && slot < static_cast<int32_t>(session.player_slots.size())) {
                session.player_slots[slot] = nullptr;
                // Only decrement before round starts; count is unused after.
                if (!session.round_started) session.connected_count--;
            }
            server_log.info("close",
                            "slot=" + std::to_string(slot) +
                            " code=" + std::to_string(code) +
                            " total=" + std::to_string(session.connections.size()));
        }
    });

    // ── HTTP: POST /api/log — accepts frontend log entries ────────────────
    // AGENT-CTX: The frontend logger POSTs JSON arrays of log entries here.
    // uWS HTTP bodies arrive in chunks; we accumulate in a per-request std::string.
    // This endpoint is dev-only — no auth, no rate limiting, no size cap.
    // If it causes issues in later slices, gate it behind a --dev flag in config.
    app.post("/api/log", [&](auto* res, auto* /*req*/) {
        auto body_buf = std::make_shared<std::string>();
        body_buf->reserve(4096);

        res->onData([res, body_buf, &frontend_log](std::string_view chunk, bool last) {
            constexpr std::size_t kMaxBody = 1 * 1024 * 1024;  // 1 MB
            if (body_buf->size() + chunk.size() > kMaxBody) {
                res->close();
                return;
            }
            *body_buf += chunk;
            if (!last) return;

            try {
                const auto arr = nlohmann::json::parse(*body_buf);
                if (arr.is_array()) {
                    for (const auto& entry : arr) {
                        const std::string level   = entry.value("level",     "INFO");
                        const std::string comp    = entry.value("component", "frontend");
                        const std::string message = entry.value("message",   "");
                        const std::string data    = entry.contains("data") && !entry["data"].is_null()
                            ? " data=" + entry["data"].dump() : "";
                        if (level == "ERROR")
                            frontend_log.error(comp, message + data);
                        else if (level == "WARN")
                            frontend_log.warn(comp, message + data);
                        else if (level == "DEBUG")
                            frontend_log.debug(comp, message + data);
                        else
                            frontend_log.info(comp, message + data);
                    }
                }
            } catch (const std::exception& ex) {
                frontend_log.warn("/api/log",
                                  std::string("parse error: ") + ex.what());
            }

            res->cork([res]() {
                res->writeHeader("Content-Type", "text/plain")
                   ->writeHeader("Access-Control-Allow-Origin", "*")
                   ->end("ok");
            });
        });

        res->onAborted([body_buf]() {
            // body_buf captured to extend shared_ptr lifetime; freed on abort.
        });
    });

    // ── OPTIONS /api/log — CORS preflight ─────────────────────────────────
    app.options("/api/log", [](auto* res, auto* /*req*/) {
        res->writeHeader("Access-Control-Allow-Origin", "*")
           ->writeHeader("Access-Control-Allow-Methods", "POST, OPTIONS")
           ->writeHeader("Access-Control-Allow-Headers", "Content-Type")
           ->end("");
    });

    bool listen_ok = false;
    app.listen(cfg_.host, cfg_.port, [&](auto* token) {
        if (token) {
            listen_ok = true;
            server_log.info("startup",
                            "listening on " + cfg_.host + ":" + std::to_string(cfg_.port));
            std::cout << "[server] listening on "
                      << cfg_.host << ":" << cfg_.port << '\n';
        } else {
            server_log.error("startup",
                             "failed to listen on port " + std::to_string(cfg_.port));
            std::cerr << "[server] failed to listen on port " << cfg_.port << '\n';
        }
    });
    if (!listen_ok) {
        throw std::runtime_error("failed to listen on port " + std::to_string(cfg_.port));
    }

    // ── Heartbeat thread (disabled — uncomment to re-enable for debugging) ──
    // Note: server_ts is captured before loop->defer executes, so it slightly
    // lags the actual broadcast time. Acceptable for debug purposes.
    //
    // std::thread heartbeat_thread([&] {
    //     while (running) {
    //         std::this_thread::sleep_for(
    //             std::chrono::milliseconds(cfg_.heartbeat_interval_ms));
    //         if (!running) break;
    //         const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
    //             std::chrono::system_clock::now().time_since_epoch()
    //         ).count();
    //         loop->defer([&session, ts, &server_log] {
    //             if (session.connections.empty()) return;
    //             const std::string payload =
    //                 nlohmann::json{{"type","heartbeat"},{"server_ts",ts}}.dump();
    //             for (WsHandle ws : session.connections)
    //                 ws->send(payload, uWS::OpCode::TEXT);
    //             server_log.debug("heartbeat",
    //                              "broadcast to " +
    //                              std::to_string(session.connections.size()) + " client(s)");
    //         });
    //     }
    // });

    app.run();
    running = false;
    // heartbeat_thread.join();
    // Join before locals (session, rng, loggers) are destroyed.
    if (countdown_thread.has_value() && countdown_thread->joinable())
        countdown_thread->join();
    server_log.info("shutdown", "server stopped");
}

} // namespace anjeer::server
