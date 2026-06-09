/**
 * Anjeer C++ script template — Boost.Beast + Boost.Asio async WebSocket
 *
 * HOW TO USE
 * ----------
 * 1. Run `anjeer setup` once to store your API key and server URLs.
 * 2. Run `anjeer join <CODE>` or `anjeer start <CODE>` to enter a lobby.
 *    The CLI joins the lobby for you, then launches this script with the
 *    correct environment variables already set. Do NOT call the join
 *    HTTP endpoint — the lobby is already starting or active and the
 *    request will return 409 GAME_ALREADY_STARTED.
 * 3. Fill in your strategy inside the handler functions below.
 *
 * ENVIRONMENT VARIABLES (set by the CLI before your script runs)
 * --------------------------------------------------------------
 *   ANJEER_API_KEY         — Bearer token for WebSocket auth
 *   ANJEER_SERVER_WS_URL   — WebSocket URL, e.g. ws://localhost:9001/ws
 *   ANJEER_HTTP_URL        — HTTP base URL, e.g. http://localhost:8080
 *   ANJEER_LOBBY_CODE      — Lobby code your script is participating in
 *   ANJEER_GAME_MODE       — Game mode: simple | intermediate | advanced
 *                            Determines which market-data messages you receive:
 *                              simple       → book_update (MBP-1, best bid/ask only)
 *                              intermediate → book_depth + book_depth_snapshot (MBP-N)
 *                              advanced     → order_added/executed/cancelled/book_snapshot (MBO)
 *
 * DEPENDENCIES
 * ------------
 *   Boost (beast, asio) >= 1.81
 *   nlohmann/json >= 3.11   (header-only: https://github.com/nlohmann/json)
 *
 * BUILD
 * -----
 *   g++ -std=c++17 -O2 anjeer_template.cpp -lboost_system -lpthread -o anjeer_bot
 *   ./anjeer_bot   # or let `anjeer start` launch it automatically
 */

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <iostream>
#include <string>
#include <unordered_map>

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace asio      = boost::asio;
using     json      = nlohmann::json;
using     tcp       = asio::ip::tcp;

// ── Configuration ─────────────────────────────────────────────────────────────
// These env vars are set automatically by `anjeer join` / `anjeer start`.
// Do not call the join HTTP endpoint in this script — the CLI owns that step.

static std::string getenv_or_exit(const char* name) {
    const char* val = std::getenv(name);
    if (!val || *val == '\0') {
        std::cerr << "ERROR: " << name
                  << " not set. Launch via `anjeer join` or set the env var.\n";
        std::exit(1);
    }
    return val;
}

static std::string getenv_or(const char* name, const char* fallback) {
    const char* val = std::getenv(name);
    return (val && *val) ? val : fallback;
}

// ── State ─────────────────────────────────────────────────────────────────────

struct BotState {
    int    player_id     = -1;
    int    my_slot       = -1;
    json   hand          = {};
    double balance       = 0.0;
    json   my_orders     = {};   // order_id → {suit, side, price, qty_remaining}
    bool   in_round      = false;
    int    rounds_played = 0;

    // MBP-1 / MBP-N state (simple + intermediate)
    json   books         = {};   // suit → {best_bid, best_ask, ...}
    json   depth         = {};   // suit → {bids: [...], asks: [...]}

    // MBO state (advanced only)
    json   order_book    = {};   // order_id → {suit, side, price, qty_remaining, player_slot}
};

static BotState         g_state;
static std::string      g_game_mode;
static std::atomic_bool g_shutdown{false};

// ── Session ───────────────────────────────────────────────────────────────────

class Session : public std::enable_shared_from_this<Session> {
public:
    explicit Session(asio::io_context& ioc)
        : resolver_(ioc), ws_(ioc) {}

    void run(const std::string& host, const std::string& port,
             const std::string& path, const std::string& api_key) {
        host_    = host;
        path_    = path;
        api_key_ = api_key;
        resolver_.async_resolve(host, port,
            [self = shared_from_this()](beast::error_code ec,
                                        tcp::resolver::results_type results) {
                self->on_resolve(ec, results);
            });
    }

    void send(const json& msg) {
        auto text = std::make_shared<std::string>(msg.dump());
        ws_.async_write(asio::buffer(*text),
            [text](beast::error_code ec, std::size_t) {
                if (ec) std::cerr << "[send error] " << ec.message() << '\n';
            });
    }

private:
    void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
        if (ec) { std::cerr << "[resolve] " << ec.message() << '\n'; return; }
        beast::get_lowest_layer(ws_).async_connect(results,
            [self = shared_from_this()](beast::error_code ec, tcp::endpoint) {
                self->on_connect(ec);
            });
    }

    void on_connect(beast::error_code ec) {
        if (ec) { std::cerr << "[connect] " << ec.message() << '\n'; return; }
        ws_.set_option(websocket::stream_base::decorator(
            [this](websocket::request_type& req) {
                req.set(beast::http::field::authorization, "Bearer " + api_key_);
            }));
        ws_.async_handshake(host_, path_,
            [self = shared_from_this()](beast::error_code ec) {
                self->on_handshake(ec);
            });
    }

    void on_handshake(beast::error_code ec) {
        if (ec) { std::cerr << "[handshake] " << ec.message() << '\n'; return; }
        std::cout << "Connected. Waiting for player_hello …\n";
        do_read();
    }

    void do_read() {
        if (g_shutdown.load()) {
            ws_.async_close(websocket::close_code::normal, [](beast::error_code){});
            return;
        }
        ws_.async_read(buf_,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                self->on_read(ec);
            });
    }

    void on_read(beast::error_code ec) {
        if (ec == websocket::error::closed || ec == asio::error::eof) {
            std::cout << "\nDisconnected.\n";
            return;
        }
        if (ec) { std::cerr << "[read] " << ec.message() << '\n'; return; }

        try {
            auto msg = json::parse(beast::buffers_to_string(buf_.data()));
            buf_.consume(buf_.size());
            dispatch(msg);
        } catch (const json::exception& e) {
            std::cerr << "[warn] JSON parse error: " << e.what() << '\n';
            buf_.consume(buf_.size());
        }

        do_read();
    }

    void dispatch(const json& msg);

    std::string host_, path_, api_key_;
    tcp::resolver resolver_;
    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer buf_;
};

// ── Shared handlers (all modes) ───────────────────────────────────────────────

static std::shared_ptr<Session> g_session;

static void on_player_hello(const json& msg) {
    g_state.player_id = msg.value("player_id", -1);
    std::cout << "[hello] player_id=" << g_state.player_id
              << " game_mode=" << g_game_mode << '\n';
}

static void on_round_start(const json& msg) {
    g_state.hand      = msg.value("hand", json{});
    g_state.balance   = msg.value("balance", 0.0);
    g_state.my_slot   = msg.value("player_slot", -1);
    g_state.in_round  = true;
    g_state.my_orders = {};
    g_state.books     = {};
    g_state.depth     = {};
    g_state.order_book = {};
    std::cout << "[round_start] slot=" << g_state.my_slot
              << " balance=" << g_state.balance
              << " mode=" << msg.value("game_mode", g_game_mode) << '\n';
    // TODO: add your opening strategy here
}

static void on_trade(const json& msg) {
    // In simple/intermediate mode, wipe_on_trade=true: all books clear after every trade.
    // In advanced mode, wipe_on_trade=false: only matched orders are removed.
    if (g_game_mode != "advanced") {
        g_state.my_orders = {};
    }
    std::cout << "[trade] suit="  << msg.value("suit",  "")
              << " price="        << msg.value("price",  0)
              << " your_side="    << msg.value("your_side", "null") << '\n';
    // TODO: re-post standing orders if desired (simple/intermediate only)
}

static void on_order_ack(const json& msg) {
    auto id = msg.value("order_id", 0);
    g_state.my_orders[std::to_string(id)] = {
        {"suit",          msg["suit"]},
        {"side",          msg["side"]},
        {"price",         msg["price"]},
        {"qty_remaining", msg.value("qty_remaining", 1)},
    };
    std::cout << "[ack] " << msg.value("side","") << ' ' << msg.value("suit","")
              << '@'      << msg.value("price", 0)
              << " qty="  << msg.value("qty_remaining", 1)
              << " id="   << id << '\n';
}

static void on_order_cancel_ack(const json& msg) {
    g_state.my_orders.erase(std::to_string(msg.value("order_id", 0)));
}

static void on_order_partially_filled(const json& msg) {
    // Advanced mode only: your resting order was partially filled.
    auto id  = msg.value("order_id", 0);
    auto key = std::to_string(id);
    auto qty_remaining = msg.value("qty_remaining", 0);
    if (g_state.my_orders.contains(key))
        g_state.my_orders[key]["qty_remaining"] = qty_remaining;
    std::cout << "[partial_fill] id=" << id
              << " filled="    << msg.value("qty_filled", 1)
              << " remaining=" << qty_remaining << '\n';
    // TODO: decide whether to re-price or let the remainder rest
}

static void on_round_end(const json& msg) {
    g_state.in_round  = false;
    g_state.my_orders = {};
    ++g_state.rounds_played;
    std::cout << "[round_end] goal=" << msg.value("goal_suit", "")
              << " (rounds played: " << g_state.rounds_played << ")\n";
    for (const auto& r : msg.value("results", json::array()))
        std::cout << "  slot="    << r.value("player_slot", 0)
                  << " payout="   << r.value("payout",      0)
                  << " balance="  << r.value("balance",     0.0) << '\n';
}

static void on_inter_round(const json& msg) {
    std::cout << "[inter_round] round=" << msg.value("round_number",      0)
              << " next in "            << msg.value("inter_round_seconds", 0) << "s\n";
    // TODO: call g_session->send({{"type","vote_to_end"}}) to end the game early
}

static void on_game_ended(const json& msg) {
    std::cout << "[game_ended]\n";
    for (const auto& r : msg.value("final_standings", json::array()))
        std::cout << "  slot="    << r.value("player_slot", 0)
                  << " balance="  << r.value("balance",     0.0) << '\n';
    g_shutdown.store(true);
}

static void on_error_msg(const json& msg) {
    std::cerr << "[error] " << msg.value("code", "") << ": "
                            << msg.value("message", "") << '\n';
}

static void on_session_error(const json& msg) {
    std::cerr << "[session_error] " << msg.value("message", "") << '\n';
    g_shutdown.store(true);
}

// ── MBP-1 handlers (simple mode) ─────────────────────────────────────────────

static void on_book_update(const json& msg) {
    auto suit = msg.value("suit", "");
    g_state.books[suit] = {
        {"best_bid",      msg["best_bid"]},
        {"best_ask",      msg["best_ask"]},
        {"best_bid_slot", msg["best_bid_slot"]},
        {"best_ask_slot", msg["best_ask_slot"]},
    };
    // TODO: react to price changes
}

// ── MBP-N handlers (intermediate mode) ───────────────────────────────────────

static void on_book_depth(const json& msg) {
    // N-level price ladder update for one side of one suit.
    auto suit = msg.value("suit", "");
    auto side = msg.value("side", "");  // "bid" or "ask"
    if (!g_state.depth.contains(suit))
        g_state.depth[suit] = {{"bids", json::array()}, {"asks", json::array()}};
    g_state.depth[suit][side + "s"] = msg.value("levels", json::array());
    // TODO: compute fair value or liquidity from the depth
}

static void on_book_depth_snapshot(const json& msg) {
    // Full N-level snapshot broadcast at round start.
    auto suit = msg.value("suit", "");
    g_state.depth[suit] = {
        {"bids", msg.value("bids", json::array())},
        {"asks", msg.value("asks", json::array())},
    };
}

// ── MBO handlers (advanced mode) ─────────────────────────────────────────────

static void on_order_added(const json& msg) {
    // A new resting order is visible in the book.
    auto id = msg.value("order_id", 0);
    g_state.order_book[std::to_string(id)] = {
        {"suit",          msg["suit"]},
        {"side",          msg["side"]},
        {"price",         msg["price"]},
        {"qty_remaining", msg.value("qty_remaining", 1)},
        {"player_slot",   msg.value("player_slot",  -1)},
        {"seq",           msg.value("seq",           0)},
    };
    // TODO: update your order-book model
}

static void on_order_executed(const json& msg) {
    // A resting order was fully or partially filled.
    auto id  = msg.value("order_id", 0);
    auto key = std::to_string(id);
    auto qty_remaining = msg.value("qty_remaining", 0);
    if (qty_remaining == 0) {
        g_state.order_book.erase(key);
    } else if (g_state.order_book.contains(key)) {
        g_state.order_book[key]["qty_remaining"] = qty_remaining;
    }
    std::cout << "[executed] id=" << id
              << " price="       << msg.value("price", 0)
              << " qty_filled="  << msg.value("qty_filled", 1) << '\n';
}

static void on_order_cancelled(const json& msg) {
    // A resting order was cancelled.
    g_state.order_book.erase(std::to_string(msg.value("order_id", 0)));
}

static void on_order_book_snapshot(const json& msg) {
    // Full MBO snapshot broadcast at round start.
    g_state.order_book = {};
    for (const auto& entry : msg.value("orders", json::array())) {
        auto id = entry.value("order_id", 0);
        g_state.order_book[std::to_string(id)] = {
            {"suit",          entry["suit"]},
            {"side",          entry["side"]},
            {"price",         entry["price"]},
            {"qty_remaining", entry.value("qty_remaining", 1)},
            {"player_slot",   entry.value("player_slot",  -1)},
        };
    }
}

// ── Dispatch impl ─────────────────────────────────────────────────────────────

void Session::dispatch(const json& msg) {
    using Handler = std::function<void(const json&)>;
    static const std::unordered_map<std::string, Handler> handlers = {
        // ── shared ────────────────────────────────────────────────────────────
        {"player_hello",          on_player_hello},
        {"trade",                 on_trade},
        {"order_ack",             on_order_ack},
        {"order_cancel_ack",      on_order_cancel_ack},
        {"order_partially_filled", on_order_partially_filled},
        {"round_start",           on_round_start},
        {"round_end",             on_round_end},
        {"inter_round",           on_inter_round},
        {"game_ended",            on_game_ended},
        {"error",                 on_error_msg},
        {"session_error",         on_session_error},
        {"round_starting",        [](const json& m){ std::cout << "[countdown] starts_at=" << m.value("starts_at","") << '\n'; }},
        {"waiting_for_start",     [](const json& m){ std::cout << "[waiting] " << m.value("connected",0) << '/' << m.value("required",0) << " players\n"; }},
        {"vote_tally",            [](const json& m){ std::cout << "[vote_tally] " << m.value("votes",0) << '/' << m.value("required",0) << '\n'; }},
        {"game_player_left",      [](const json& m){ std::cout << "[player_left] slot=" << m.value("player_slot",0) << '\n'; }},
        {"all_balances",          [](const json&){}},
        {"hand_totals",           [](const json&){}},
        {"delta_update",          [](const json&){}},
        {"spectator_count",       [](const json&){}},
        {"heartbeat",             [](const json&){}},
        // ── MBP-1 (simple) ────────────────────────────────────────────────────
        {"book_update",           on_book_update},
        // ── MBP-N (intermediate) ──────────────────────────────────────────────
        {"book_depth",            on_book_depth},
        {"book_depth_snapshot",   on_book_depth_snapshot},
        // ── MBO (advanced) ────────────────────────────────────────────────────
        {"order_added",           on_order_added},
        {"order_executed",        on_order_executed},
        {"order_cancelled",       on_order_cancelled},
        {"order_book_snapshot",   on_order_book_snapshot},
    };

    const auto t  = msg.value("type", "");
    const auto it = handlers.find(t);
    if (it != handlers.end())
        it->second(msg);
    else
        std::cout << "[?] " << t << ' ' << msg.dump() << '\n';
}

// ── Helpers (call from handlers) ──────────────────────────────────────────────

inline void submit_order(const std::string& suit, const std::string& side, int price, int qty = 1) {
    g_session->send({{"type","submit_order"},{"suit",suit},{"side",side},{"price",price},{"qty",qty}});
}

inline void nudge(const std::string& suit, const std::string& side) {
    g_session->send({{"type","nudge"},{"suit",suit},{"side",side}});
}

inline void cancel_order(int order_id) {
    g_session->send({{"type","cancel_order"},{"order_id",order_id}});
}

inline void vote_to_end() {
    g_session->send({{"type","vote_to_end"}});
}

inline void script_log(const std::string& message) {
    g_session->send({{"type","script_log"},{"message",message}});
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main() {
    const auto api_key    = getenv_or_exit("ANJEER_API_KEY");
    const auto lobby_code = getenv_or_exit("ANJEER_LOBBY_CODE");
    const auto ws_url     = getenv_or("ANJEER_SERVER_WS_URL", "ws://localhost:9001/ws");
    g_game_mode           = getenv_or("ANJEER_GAME_MODE",     "simple");

    // Parse ws://host[:port]/path
    std::string host, port, path;
    {
        std::string s = ws_url;
        if (s.substr(0, 5) == "ws://") s = s.substr(5);
        auto slash    = s.find('/');
        path          = (slash == std::string::npos) ? "/" : s.substr(slash);
        auto hostport = s.substr(0, slash == std::string::npos ? s.size() : slash);
        auto colon    = hostport.find(':');
        if (colon == std::string::npos) { host = hostport; port = "9001"; }
        else { host = hostport.substr(0, colon); port = hostport.substr(colon + 1); }
    }

    std::cout << "Connecting to " << ws_url
              << " (lobby " << lobby_code << ", mode=" << g_game_mode << ") …\n";

    asio::io_context ioc;

    asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](beast::error_code, int) {
        g_shutdown.store(true);
        ioc.stop();
    });

    // The CLI has already joined the lobby and started the game before
    // launching this binary. Connect directly — do not POST to /lobbies/join.
    g_session = std::make_shared<Session>(ioc);
    g_session->run(host, port, path, api_key);

    ioc.run();
    return 0;
}
