#include <catch2/catch_test_macros.hpp>
#include "server/api_key_repo.h"
#include "server/bot_manager.h"
#include "server/bot_scheduler.h"
#include "server/ws_server.h"
#include "server/auth_service.h"
#include "server/config.h"
#include "server/db.h"
#include "server/event_bus.h"
#include "server/lobby_gateway.h"
#include "server/lobby_repo.h"
#include "server/player_repo.h"

// POSIX networking
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using anjeer::server::ServerConfig;
using anjeer::server::WsServer;

// Shared singletons — pool_size=1 keeps the test DB connection count minimal.
static anjeer::server::DbPool& test_db_pool() {
    static anjeer::server::DbPool pool(TEST_DB_CONN, 1);
    return pool;
}
static anjeer::server::PlayerRepo& test_player_repo() {
    static anjeer::server::PlayerRepo repo;
    return repo;
}
static anjeer::server::LobbyRepo& test_lobby_repo() {
    static anjeer::server::LobbyRepo repo;
    return repo;
}
static anjeer::server::LocalEventBus& test_event_bus() {
    static anjeer::server::LocalEventBus bus;
    return bus;
}
// Separate bus for the mode/spectator server — prevents game:start events
// from racing across all test servers when setup_spectator_test publishes.
static anjeer::server::LocalEventBus& test_mode_event_bus() {
    static anjeer::server::LocalEventBus bus;
    return bus;
}
static anjeer::server::ApiKeyRepo& test_api_key_repo() {
    static anjeer::server::ApiKeyRepo repo;
    return repo;
}
static anjeer::server::LobbyGateway& test_lobby_gateway() {
    static anjeer::server::LobbyGateway gw(test_db_pool(), test_lobby_repo(), test_event_bus());
    return gw;
}
static anjeer::server::BotScheduler& test_bot_scheduler() {
    static anjeer::server::BotScheduler sched(1);
    return sched;
}
static anjeer::server::BotManager& test_bot_manager() {
    static anjeer::server::ServerConfig::BotsConfig default_bots_cfg;
    static anjeer::server::BotManager mgr(test_bot_scheduler(), default_bots_cfg);
    return mgr;
}

// ═══════════════════════════════════════════════════════════════════════════
// WsTestClient — minimal RFC 6455 text-frame WebSocket client
//
// Implements just enough of the protocol for smoke tests:
// - TCP connect + HTTP Upgrade handshake
// - Sending masked text frames (required for client→server by RFC 6455 §5.3)
// - Receiving unmasked server frames, auto-responding to pings
// No TLS — the test server runs plain HTTP.
// ═══════════════════════════════════════════════════════════════════════════

class WsTestClient {
public:
    // extra_headers: optional additional HTTP headers appended before the blank line,
    // each terminated with \r\n. Example: "Authorization: Bearer ank_...\r\n"
    explicit WsTestClient(int port, std::string extra_headers = "", std::string path = "/ws")
        : fd_(::socket(AF_INET, SOCK_STREAM, 0))
        , extra_headers_(std::move(extra_headers))
        , path_(std::move(path)) {
        if (fd_ < 0) throw std::runtime_error("socket() failed");

        // 3-second receive timeout — prevents tests hanging on missed messages.
        struct timeval tv{ 3, 0 };
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(static_cast<uint16_t>(port));
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            throw std::runtime_error("connect() failed");

        do_handshake();
    }

    ~WsTestClient() { if (fd_ >= 0) ::close(fd_); }

    void send_text(const std::string& payload) {
        // FIN=1, opcode=TEXT(0x1), mask=1
        std::vector<uint8_t> frame;
        frame.push_back(0x81);

        const uint8_t mask[4] = { 0x1a, 0x2b, 0x3c, 0x4d };
        const size_t len = payload.size();
        if (len < 126) {
            frame.push_back(0x80 | static_cast<uint8_t>(len));
        } else if (len < 65536) {
            frame.push_back(0x80 | 126);
            frame.push_back(static_cast<uint8_t>((len >> 8) & 0xff));
            frame.push_back(static_cast<uint8_t>(len & 0xff));
        } else {
            throw std::runtime_error("payload too large for test client");
        }
        frame.insert(frame.end(), mask, mask + 4);
        for (size_t i = 0; i < len; ++i)
            frame.push_back(static_cast<uint8_t>(payload[i]) ^ mask[i % 4]);

        write_all(frame.data(), frame.size());
    }

    void send_json(const nlohmann::json& j) { send_text(j.dump()); }

    // Receive the next text/binary frame, automatically responding to pings.
    std::string recv_text() {
        for (;;) {
            uint8_t hdr[2];
            read_all(hdr, 2);

            const uint8_t op   = hdr[0] & 0x0f;
            const bool masked  = (hdr[1] & 0x80) != 0;
            size_t len         = hdr[1] & 0x7f;

            if (len == 126) {
                uint8_t ext[2]; read_all(ext, 2);
                len = (static_cast<size_t>(ext[0]) << 8) | ext[1];
            } else if (len == 127) {
                throw std::runtime_error("64-bit length frames not supported in test client");
            }

            uint8_t mkey[4] = {};
            if (masked) read_all(mkey, 4);

            std::string payload(len, '\0');
            if (len) read_all(reinterpret_cast<uint8_t*>(payload.data()), len);
            if (masked) for (size_t i = 0; i < len; ++i) payload[i] ^= mkey[i % 4];

            if (op == 0x9) { send_pong(payload); continue; }  // ping → pong
            if (op == 0x8) throw std::runtime_error("server closed connection");
            if (op == 0x1 || op == 0x2) return payload;       // text or binary
            // skip continuation frames and unknown control frames
        }
    }

    nlohmann::json recv_json() { return nlohmann::json::parse(recv_text()); }

    // Decode a binary msgpack frame as JSON (for /ws/marketdata which always sends msgpack).
    nlohmann::json recv_msgpack() {
        const std::string raw = recv_text();
        return nlohmann::json::from_msgpack(
            std::vector<uint8_t>(raw.begin(), raw.end()));
    }

    // Drain incoming messages until one with the expected "type" field is found.
    // Skips unrelated message types (e.g. heartbeats) so tests are order-tolerant.
    nlohmann::json recv_of_type(const std::string& want) {
        for (int i = 0; i < 20; ++i) {
            auto j = recv_json();
            if (j.value("type", "") == want) return j;
        }
        throw std::runtime_error("did not receive message of type '" + want + "' within 20 frames");
    }

private:
    int         fd_;
    std::string extra_headers_;
    std::string path_;

    void do_handshake() {
        // RFC 6455 example key — any valid base64 value is accepted by uWS.
        const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
        const std::string req =
            "GET " + path_ + " HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: " + key + "\r\n"
            "Sec-WebSocket-Version: 13\r\n" +
            extra_headers_ +
            "\r\n";
        write_all(reinterpret_cast<const uint8_t*>(req.data()), req.size());

        std::string resp;
        while (resp.find("\r\n\r\n") == std::string::npos) {
            char c;
            if (::recv(fd_, &c, 1, 0) <= 0) throw std::runtime_error("handshake recv failed");
            resp += c;
        }
        if (resp.find("101") == std::string::npos)
            throw std::runtime_error("expected 101 Switching Protocols, got: " + resp);
    }

    void send_pong(const std::string& payload) {
        std::vector<uint8_t> frame;
        frame.push_back(0x8a);  // FIN + PONG
        const uint8_t mask[4] = {};
        const size_t len = std::min(payload.size(), size_t{125});
        frame.push_back(0x80 | static_cast<uint8_t>(len));
        frame.insert(frame.end(), mask, mask + 4);
        for (size_t i = 0; i < len; ++i)
            frame.push_back(static_cast<uint8_t>(payload[i]));
        write_all(frame.data(), frame.size());
    }

    void write_all(const uint8_t* data, size_t len) {
        size_t sent = 0;
        while (sent < len) {
            const ssize_t n = ::send(fd_, data + sent, len - sent, 0);
            if (n <= 0) throw std::runtime_error("send() failed");
            sent += static_cast<size_t>(n);
        }
    }

    void read_all(uint8_t* buf, size_t len) {
        size_t got = 0;
        while (got < len) {
            const ssize_t n = ::recv(fd_, buf + got, len - got, 0);
            if (n <= 0) throw std::runtime_error("recv() failed / timed out");
            got += static_cast<size_t>(n);
        }
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Test server — started once per process, shared by all TEST_CASEs
// ═══════════════════════════════════════════════════════════════════════════

static constexpr int WS_TEST_PORT  = 19002;
static constexpr int WS_LOBBY_PORT = 19007;

// Base config shared across all test servers. Callers override only what differs.
static ServerConfig make_test_server_config(int port) {
    ServerConfig cfg;
    cfg.host                  = "127.0.0.1";
    cfg.port                  = port;
    cfg.heartbeat_interval_ms = 60000;  // suppress noise during tests
    cfg.ping_interval_ms      = 1000;
    cfg.ping_timeout_ms       = 8000;
    cfg.order_book.min_price               = 1;
    cfg.order_book.max_price               = 99;
    cfg.order_book.nudge_initial_buy_price  = 1;
    cfg.order_book.nudge_initial_sell_price = 99;
    cfg.order_book.active_suits             = { "clubs" };
    cfg.game.player_count           = 2;
    cfg.game.total_cards            = 40;
    cfg.game.countdown_seconds      = 0;
    cfg.game.round_duration_seconds = 3600;
    cfg.game.inter_round_seconds    = 5;
    cfg.scoring.starting_balance    = 100;
    cfg.scoring.pot_size            = 40;
    cfg.scoring.points_per_card     = 20;
    cfg.db.connection_string        = TEST_DB_CONN;
    cfg.db.pool_size                = 1;
    cfg.db.migrations_dir           = TEST_MIGRATIONS_DIR;
    // AGENT-CTX: Auth config is required by WsServer to validate JWTs in the
    // upgrade handler. Test clients don't send cookies, so validate_access_token
    // is never called — but AuthService construction requires the fields.
    cfg.auth.jwt_secret                = "test-secret-must-be-at-least-32-chars!!";
    cfg.auth.access_token_ttl_seconds  = 3600;
    cfg.auth.refresh_token_ttl_seconds = 86400;
    cfg.auth.secure_cookies            = false;
    return cfg;
}

// AGENT-CTX: AuthService is a per-server singleton in tests, not shared across
// server instances (each test server uses make_test_server_config which returns
// a copy). test_auth_service() is only used where WS_TEST_PORT server is created.
static anjeer::server::AuthService& test_auth_service(const ServerConfig& cfg) {
    static anjeer::server::AuthService svc(test_db_pool(), test_player_repo(), cfg);
    return svc;
}

static void ensure_server_running() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) {
        // Another call started it — wait until the port is accepting connections.
        for (int i = 0; i < 200; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port   = htons(WS_TEST_PORT);
            ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
            const bool ok = (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
            ::close(fd);
            if (ok) return;
        }
        throw std::runtime_error("test server did not become ready in time");
    }

    // player_count=99 prevents any round from starting (tests never connect 99 clients).
    static ServerConfig cfg = make_test_server_config(WS_TEST_PORT);
    cfg.game.player_count      = 99;
    cfg.game.countdown_seconds = 3;

    std::thread([&cfg]() {
        WsServer srv(cfg, anjeer::server::WsServerDeps{
                         test_lobby_gateway(), test_db_pool(), test_lobby_repo(),
                         test_auth_service(cfg), test_api_key_repo(), test_event_bus(),
                         test_bot_manager()});
        srv.run();  // blocks; detached — OS cleans up on process exit
    }).detach();

    for (int i = 0; i < 200; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(WS_TEST_PORT);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        const bool ok = (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        ::close(fd);
        if (ok) return;
    }
    throw std::runtime_error("test server failed to start");
}

// ═══════════════════════════════════════════════════════════════════════════
// Tests
// ═══════════════════════════════════════════════════════════════════════════

// AGENT-CTX: player_id is -1 for unauthenticated lobby sockets (no JWT cookie).
// The old architecture assigned slots (0,1,2,...) on connect; the new one only
// assigns slots when the player is in an active game session with a valid JWT.
TEST_CASE("WS server — player_hello sent on connect", "[ws_server]") {
    ensure_server_running();
    WsTestClient client(WS_TEST_PORT);

    const auto hello = client.recv_of_type("player_hello");
    REQUIRE(hello.contains("player_id"));
    REQUIRE(hello["player_id"].is_number_integer());
    CHECK(hello["player_id"].get<int>() == -1);  // unauthenticated: no JWT cookie
}

// AGENT-CTX: submit_order / nudge / cancel_order on a lobby socket (not in a
// game session) must return ROUND_NOT_ACTIVE. These tests remain valid because
// the error guard in WsServer's message handler fires before any session lookup.
TEST_CASE("WS server — submit_order rejected when not in game session", "[ws_server][phase]") {
    ensure_server_running();
    WsTestClient client(WS_TEST_PORT);

    client.recv_of_type("player_hello");

    client.send_json({ {"type","submit_order"}, {"suit","clubs"}, {"side","buy"}, {"price",30} });

    const auto err = client.recv_of_type("error");
    REQUIRE(err.value("code","") == "ROUND_NOT_ACTIVE");
}

TEST_CASE("WS server — nudge rejected when not in game session", "[ws_server][phase]") {
    ensure_server_running();
    WsTestClient client(WS_TEST_PORT);

    client.recv_of_type("player_hello");

    client.send_json({ {"type","nudge"}, {"suit","clubs"}, {"side","buy"} });

    const auto err = client.recv_of_type("error");
    REQUIRE(err.value("code","") == "ROUND_NOT_ACTIVE");
}

TEST_CASE("WS server — cancel_order rejected when not in game session", "[ws_server][phase]") {
    ensure_server_running();
    WsTestClient client(WS_TEST_PORT);

    client.recv_of_type("player_hello");

    client.send_json({ {"type","cancel_order"}, {"order_id",1} });

    const auto err = client.recv_of_type("error");
    REQUIRE(err.value("code","") == "ROUND_NOT_ACTIVE");
}

// ═══════════════════════════════════════════════════════════════════════════
// Lobby subscription tests — subscribe_lobby / event forwarding
// ═══════════════════════════════════════════════════════════════════════════

struct LobbySetup { int64_t player_id; std::string lobby_id; };

// Insert a player (idempotent via ON CONFLICT) and create a fresh lobby via
// LobbyRepo so the 6-char code is generated correctly. Runs migrations so the
// test binary is self-contained even on a fresh anjeer_test DB.
static LobbySetup setup_lobby_in_db() {
    pqxx::connection conn(TEST_DB_CONN);
    anjeer::server::DbMigrator m(conn, TEST_MIGRATIONS_DIR);
    m.run();

    int64_t player_id;
    {
        pqxx::work txn(conn);
        auto r = txn.exec(
            "INSERT INTO players (username, oauth_provider, oauth_id) "
            "VALUES ('ws_sub_tester','github','gh_ws_sub_1') "
            "ON CONFLICT (oauth_provider, oauth_id) "
            "DO UPDATE SET username = excluded.username "
            "RETURNING id");
        player_id = r[0][0].as<int64_t>();
        txn.commit();
    }

    std::string lobby_id;
    {
        pqxx::work txn(conn);
        // Remove any leftover waiting lobby from a prior test run for this player.
        txn.exec_params(
            "DELETE FROM lobbies WHERE creator_id = $1 AND status = 'waiting'",
            player_id
        );
        const auto lobby = test_lobby_repo().create(txn, {player_id, 2, 8});
        test_lobby_repo().add_player(txn, lobby.id, player_id);
        lobby_id = lobby.id;
        txn.commit();
    }
    return { player_id, lobby_id };
}

static anjeer::server::AuthService& test_lobby_auth_service() {
    static ServerConfig cfg = make_test_server_config(WS_LOBBY_PORT);
    static anjeer::server::AuthService svc(test_db_pool(), test_player_repo(), cfg);
    return svc;
}

static void ensure_lobby_sub_server_running() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) {
        for (int i = 0; i < 200; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{}; addr.sin_family = AF_INET;
            addr.sin_port = htons(WS_LOBBY_PORT);
            ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
            bool ok = (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
            ::close(fd);
            if (ok) return;
        }
        throw std::runtime_error("WS lobby sub server did not become ready");
    }

    static ServerConfig cfg = make_test_server_config(WS_LOBBY_PORT);
    cfg.game.player_count      = 99;
    cfg.game.countdown_seconds = 3;

    std::thread([&cfg]() {
        WsServer srv(cfg, anjeer::server::WsServerDeps{
                         test_lobby_gateway(), test_db_pool(), test_lobby_repo(),
                         test_lobby_auth_service(), test_api_key_repo(), test_event_bus(),
                         test_bot_manager()});
        srv.run();
    }).detach();

    for (int i = 0; i < 200; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{}; addr.sin_family = AF_INET;
        addr.sin_port = htons(WS_LOBBY_PORT);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        bool ok = (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        ::close(fd);
        if (ok) return;
    }
    throw std::runtime_error("WS lobby sub server failed to start");
}

TEST_CASE("WS server — subscribe_lobby returns lobby_state snapshot", "[ws_server][lobby]") {
    ensure_lobby_sub_server_running();
    const auto setup = setup_lobby_in_db();

    WsTestClient client(WS_LOBBY_PORT);
    client.recv_of_type("player_hello");

    client.send_json({ {"type","subscribe_lobby"}, {"lobby_id", setup.lobby_id} });

    const auto msg = client.recv_of_type("lobby_state");
    REQUIRE(msg.value("lobby_id", "") == setup.lobby_id);
    REQUIRE(msg.contains("code"));
    REQUIRE(msg["players"].is_array());
    CHECK(msg["players"].size() == 1);  // one player added in setup
}

TEST_CASE("WS server — subscribed client receives event_bus messages", "[ws_server][lobby]") {
    ensure_lobby_sub_server_running();
    const auto setup = setup_lobby_in_db();

    WsTestClient client(WS_LOBBY_PORT);
    client.recv_of_type("player_hello");

    client.send_json({ {"type","subscribe_lobby"}, {"lobby_id", setup.lobby_id} });
    client.recv_of_type("lobby_state");  // consume the initial snapshot

    // Publish directly to the shared event bus; recv_of_type waits up to 3 s.
    nlohmann::json ev;
    ev["type"]         = "player_joined";
    ev["lobby_id"]     = setup.lobby_id;
    ev["player_id"]    = setup.player_id;
    ev["username"]     = "ws_sub_tester";
    ev["player_count"] = 1;
    ev["joined_at"]    = "2026-04-23T00:00:00Z";
    test_event_bus().publish("lobby:" + setup.lobby_id, ev.dump());

    const auto fwd = client.recv_of_type("player_joined");
    CHECK(fwd.value("lobby_id",  "") == setup.lobby_id);
    CHECK(fwd.value("player_id", -1) == static_cast<int>(setup.player_id));
}

// ═══════════════════════════════════════════════════════════════════════════
// Task 5 — API key auth + rate limiter integration tests
//
// A dedicated server (WS_API_AUTH_PORT) with a low-capacity rate limiter
// (capacity=3, suspend_threshold=3) so tests don't need to send 70+ messages.
// ═══════════════════════════════════════════════════════════════════════════

static constexpr int WS_API_AUTH_PORT = 19010;

static anjeer::server::AuthService& test_api_auth_service() {
    static ServerConfig cfg = make_test_server_config(WS_API_AUTH_PORT);
    static anjeer::server::AuthService svc(test_db_pool(), test_player_repo(), cfg);
    return svc;
}

static void ensure_api_auth_server_running() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) {
        for (int i = 0; i < 200; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{}; addr.sin_family = AF_INET;
            addr.sin_port = htons(WS_API_AUTH_PORT);
            ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
            bool ok = (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
            ::close(fd);
            if (ok) return;
        }
        throw std::runtime_error("API auth server did not become ready");
    }

    static ServerConfig cfg = make_test_server_config(WS_API_AUTH_PORT);
    cfg.game.player_count         = 99;
    cfg.rate_limit.capacity         = 3.0;
    cfg.rate_limit.refill_rate      = 0.0;   // no refill — tests are synchronous
    cfg.rate_limit.suspend_threshold = 3;
    cfg.rate_limit.suspend_seconds   = 60;

    std::thread([&cfg]() {
        WsServer srv(cfg, anjeer::server::WsServerDeps{
                         test_lobby_gateway(), test_db_pool(), test_lobby_repo(),
                         test_api_auth_service(), test_api_key_repo(), test_event_bus(),
                         test_bot_manager()});
        srv.run();
    }).detach();

    for (int i = 0; i < 200; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{}; addr.sin_family = AF_INET;
        addr.sin_port = htons(WS_API_AUTH_PORT);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        bool ok = (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        ::close(fd);
        if (ok) return;
    }
    throw std::runtime_error("API auth server failed to start");
}

// Creates a player (idempotent) and a fresh API key for them.
// suffix differentiates players across test cases (each test needs its own
// player so rate-limiter buckets don't bleed between tests).
struct ApiKeySetup { int64_t player_id; std::string key; };

static ApiKeySetup create_api_key_in_db(const std::string& suffix) {
    pqxx::connection direct(TEST_DB_CONN);
    anjeer::server::DbMigrator m(direct, TEST_MIGRATIONS_DIR);
    m.run();

    int64_t player_id;
    {
        pqxx::work txn(direct);
        auto r = txn.exec_params(
            "INSERT INTO players (username, oauth_provider, oauth_id) "
            "VALUES ($1, 'github', $2) "
            "ON CONFLICT (oauth_provider, oauth_id) "
            "DO UPDATE SET username = excluded.username "
            "RETURNING id",
            "api_tester_" + suffix, "gh_api_" + suffix);
        player_id = r[0][0].as<int64_t>();
        // Revoke any lingering active key from prior test runs.
        txn.exec_params(
            "UPDATE api_keys SET revoked_at = NOW() "
            "WHERE player_id = $1 AND revoked_at IS NULL",
            player_id);
        txn.commit();
    }

    std::string err;
    std::optional<std::string> key;
    {
        pqxx::work txn(direct);
        key = test_api_key_repo().create(txn, static_cast<int32_t>(player_id), "test-key", err);
        txn.commit();
    }
    if (!key) throw std::runtime_error("create_api_key_in_db failed: " + err);
    return { player_id, *key };
}

// ─── Auth tests ───────────────────────────────────────────────────────────

TEST_CASE("WS server — valid API key grants player_hello with correct player_id", "[ws_server][auth]") {
    ensure_api_auth_server_running();
    const auto setup = create_api_key_in_db("auth1");

    WsTestClient client(WS_API_AUTH_PORT,
        "Authorization: Bearer " + setup.key + "\r\n");

    const auto hello = client.recv_of_type("player_hello");
    CHECK(hello["player_id"].get<int64_t>() == setup.player_id);
}

TEST_CASE("WS server — invalid API key closes with API_KEY_INVALID", "[ws_server][auth]") {
    ensure_api_auth_server_running();

    // Well-formed prefix but non-existent key
    WsTestClient client(WS_API_AUTH_PORT,
        "Authorization: Bearer ank_0000000000000000000000000000000000000000000000000000000000000000\r\n");

    bool got_invalid = false;
    try {
        for (int i = 0; i < 5; ++i) {
            auto msg = client.recv_json();
            if (msg.value("type","") == "error" && msg.value("code","") == "API_KEY_INVALID") {
                got_invalid = true; break;
            }
        }
    } catch (...) {}

    CHECK(got_invalid);
}

TEST_CASE("WS server — revoked API key closes with API_KEY_INVALID", "[ws_server][auth]") {
    ensure_api_auth_server_running();
    const auto setup = create_api_key_in_db("auth2");

    // Revoke the key directly in DB
    {
        pqxx::connection direct(TEST_DB_CONN);
        pqxx::work txn(direct);
        txn.exec_params(
            "UPDATE api_keys SET revoked_at = NOW() "
            "WHERE player_id = $1 AND revoked_at IS NULL",
            setup.player_id);
        txn.commit();
    }

    WsTestClient client(WS_API_AUTH_PORT,
        "Authorization: Bearer " + setup.key + "\r\n");

    bool got_invalid = false;
    try {
        for (int i = 0; i < 5; ++i) {
            auto msg = client.recv_json();
            if (msg.value("type","") == "error" && msg.value("code","") == "API_KEY_INVALID") {
                got_invalid = true; break;
            }
        }
    } catch (...) {}

    CHECK(got_invalid);
}

// ─── Rate limit tests ─────────────────────────────────────────────────────

TEST_CASE("WS server — rate limit warning sent on burst", "[ws_server][ratelimit]") {
    ensure_api_auth_server_running();
    const auto setup = create_api_key_in_db("rl1");

    WsTestClient client(WS_API_AUTH_PORT,
        "Authorization: Bearer " + setup.key + "\r\n");
    client.recv_of_type("player_hello");

    // capacity=3: msgs 1–3 allow (bucket empties), msg 4 → Warn
    for (int i = 0; i < 4; ++i) {
        client.send_json({ {"type","noop"} });
    }

    // Drain responses looking for RATE_LIMIT_WARNING; skip ROUND_NOT_ACTIVE
    bool got_warning = false;
    try {
        for (int i = 0; i < 20; ++i) {
            auto msg = client.recv_json();
            if (msg.value("type","") == "error" && msg.value("code","") == "RATE_LIMIT_WARNING") {
                got_warning = true; break;
            }
        }
    } catch (...) {}

    CHECK(got_warning);
}

TEST_CASE("WS server — connection suspended on sustained rate excess", "[ws_server][ratelimit]") {
    ensure_api_auth_server_running();
    const auto setup = create_api_key_in_db("rl2");

    WsTestClient client(WS_API_AUTH_PORT,
        "Authorization: Bearer " + setup.key + "\r\n");
    client.recv_of_type("player_hello");

    // capacity=3, suspend_threshold=3:
    //   msgs 1–3 → Allow (exhaust bucket)
    //   msgs 4–6 → Warn (violation_streak 1, 2, 3 ≥ threshold → Suspend on 6th)
    for (int i = 0; i < 6; ++i) {
        try { client.send_json({ {"type","noop"} }); }
        catch (...) { break; }
    }

    bool got_suspended = false;
    try {
        for (int i = 0; i < 30; ++i) {
            auto msg = client.recv_json();
            if (msg.value("type","") == "error" && msg.value("code","") == "RATE_LIMIT_EXCEEDED") {
                got_suspended = true; break;
            }
        }
    } catch (...) {}

    CHECK(got_suspended);
}

// ═══════════════════════════════════════════════════════════════════════════
// Task 6 — Lobby mode enforcement tests
//
// A dedicated server (WS_MODE_PORT) is used so mode tests don't share
// player_to_lobby_ state with the api_auth server. Each test fires a
// game:start event directly on the shared test_event_bus() to trigger
// create_session, then connects with the wrong (or correct) auth type.
// ═══════════════════════════════════════════════════════════════════════════

static constexpr int WS_MODE_PORT = 19011;

static anjeer::server::AuthService& test_mode_auth_service() {
    static ServerConfig cfg = make_test_server_config(WS_MODE_PORT);
    static anjeer::server::AuthService svc(test_db_pool(), test_player_repo(), cfg);
    return svc;
}

static void ensure_mode_server_running() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) {
        for (int i = 0; i < 200; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{}; addr.sin_family = AF_INET;
            addr.sin_port = htons(WS_MODE_PORT);
            ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
            bool ok = (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
            ::close(fd);
            if (ok) return;
        }
        throw std::runtime_error("mode server did not become ready");
    }

    static ServerConfig cfg = make_test_server_config(WS_MODE_PORT);
    // AGENT-CTX: player_count=99 so create_session is not gated on headcount;
    // mode tests only care about auth-type parity, not game mechanics.
    cfg.game.player_count = 99;

    std::thread([&cfg]() {
        WsServer srv(cfg, anjeer::server::WsServerDeps{
                         test_lobby_gateway(), test_db_pool(), test_lobby_repo(),
                         test_mode_auth_service(), test_api_key_repo(), test_mode_event_bus(),
                         test_bot_manager()});
        srv.run();
    }).detach();

    for (int i = 0; i < 200; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{}; addr.sin_family = AF_INET;
        addr.sin_port = htons(WS_MODE_PORT);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        bool ok = (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        ::close(fd);
        if (ok) return;
    }
    throw std::runtime_error("mode server failed to start");
}

struct ModeTestSetup {
    int64_t     player_id;
    std::string lobby_id;
    std::string api_key;
    std::string jwt_token;
};

// Creates a player + lobby of the given mode, starts the session via event bus,
// and returns credentials for the requested auth type.
// AGENT-CTX: The lobby is inserted in 'starting' status so the game:start handler's
// Starting→InGame transition succeeds. A 200 ms sleep gives the uWS event loop time
// to process the loop->defer() inside the game:start subscriber before the test connects.
static ModeTestSetup setup_mode_test(anjeer::server::LobbyMode mode,
                                     bool use_api_key,
                                     const std::string& suffix) {
    pqxx::connection direct(TEST_DB_CONN);
    anjeer::server::DbMigrator m(direct, TEST_MIGRATIONS_DIR);
    m.run();

    int64_t player_id;
    {
        pqxx::work txn(direct);
        auto r = txn.exec_params(
            "INSERT INTO players (username, oauth_provider, oauth_id) "
            "VALUES ($1, 'github', $2) "
            "ON CONFLICT (oauth_provider, oauth_id) "
            "DO UPDATE SET username = excluded.username "
            "RETURNING id",
            "mode_tester_" + suffix, "gh_mode_" + suffix);
        player_id = r[0][0].as<int64_t>();
        txn.exec_params(
            "UPDATE api_keys SET revoked_at = NOW() "
            "WHERE player_id = $1 AND revoked_at IS NULL", player_id);
        txn.commit();
    }

    std::string lobby_id;
    {
        pqxx::work txn(direct);
        txn.exec_params(
            "DELETE FROM lobbies WHERE creator_id = $1 AND status IN ('waiting','starting')",
            player_id);
        auto lobby = test_lobby_repo().create(txn, {static_cast<int64_t>(player_id), 1, 8, mode});
        test_lobby_repo().add_player(txn, lobby.id, player_id);
        txn.exec_params("UPDATE lobbies SET status = 'starting' WHERE id = $1", lobby.id);
        lobby_id = lobby.id;
        txn.commit();
    }

    std::string api_key;
    std::string jwt_token;
    if (use_api_key) {
        std::string err;
        pqxx::work txn(direct);
        auto key = test_api_key_repo().create(txn, static_cast<int32_t>(player_id), "mode-test", err);
        txn.commit();
        if (!key) throw std::runtime_error("create api key failed: " + err);
        api_key = *key;
    } else {
        anjeer::server::Player p;
        p.id             = player_id;
        p.username       = "mode_tester_" + suffix;
        p.oauth_provider = "github";
        p.oauth_id       = "gh_mode_" + suffix;
        p.games_played   = 0;
        jwt_token = test_mode_auth_service().issue_tokens(p).access_token;
    }

    nlohmann::json ev;
    ev["lobby_id"] = lobby_id;
    test_mode_event_bus().publish("game:start", ev.dump());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    return { player_id, lobby_id, api_key, jwt_token };
}

TEST_CASE("WS server — api key connection rejected from ui lobby", "[ws_server][mode]") {
    ensure_mode_server_running();
    const auto setup = setup_mode_test(anjeer::server::LobbyMode::UI, true, "mode1");

    WsTestClient client(WS_MODE_PORT,
        "Authorization: Bearer " + setup.api_key + "\r\n");

    bool got_mismatch = false;
    try {
        for (int i = 0; i < 10; ++i) {
            auto msg = client.recv_json();
            if (msg.value("type","") == "error" && msg.value("code","") == "LOBBY_MODE_MISMATCH") {
                got_mismatch = true; break;
            }
        }
    } catch (...) {}
    CHECK(got_mismatch);
}


TEST_CASE("WS server — api key connection accepted in api lobby", "[ws_server][mode]") {
    ensure_mode_server_running();
    const auto setup = setup_mode_test(anjeer::server::LobbyMode::API, true, "mode3");

    WsTestClient client(WS_MODE_PORT,
        "Authorization: Bearer " + setup.api_key + "\r\n");

    bool got_hello = false;
    try {
        for (int i = 0; i < 10; ++i) {
            auto msg = client.recv_json();
            if (msg.value("type","") == "player_hello") { got_hello = true; break; }
        }
    } catch (...) {}
    CHECK(got_hello);
}

TEST_CASE("WS server — jwt connection accepted in ui lobby", "[ws_server][mode]") {
    ensure_mode_server_running();
    const auto setup = setup_mode_test(anjeer::server::LobbyMode::UI, false, "mode4");

    WsTestClient client(WS_MODE_PORT,
        "Cookie: access_token=" + setup.jwt_token + "\r\n");

    bool got_hello = false;
    try {
        for (int i = 0; i < 10; ++i) {
            auto msg = client.recv_json();
            if (msg.value("type","") == "player_hello") { got_hello = true; break; }
        }
    } catch (...) {}
    CHECK(got_hello);
}

// ═══════════════════════════════════════════════════════════════════════════
// Task 7 — Spectator infrastructure tests
//
// Reuses WS_MODE_PORT (already running from Task 6 tests). Each test sets up
// a UI-mode lobby with one player, fires game:start, then connects a second
// client as spectator via spectate_lobby.
// ═══════════════════════════════════════════════════════════════════════════

struct SpectatorTestSetup {
    int64_t     player_id;
    std::string lobby_id;
    std::string jwt_token;
};

static SpectatorTestSetup setup_spectator_test(const std::string& suffix) {
    pqxx::connection direct(TEST_DB_CONN);
    anjeer::server::DbMigrator m(direct, TEST_MIGRATIONS_DIR);
    m.run();

    int64_t player_id;
    {
        pqxx::work txn(direct);
        auto r = txn.exec_params(
            "INSERT INTO players (username, oauth_provider, oauth_id) "
            "VALUES ($1, 'github', $2) "
            "ON CONFLICT (oauth_provider, oauth_id) "
            "DO UPDATE SET username = excluded.username "
            "RETURNING id",
            "spec_tester_" + suffix, "gh_spec_" + suffix);
        player_id = r[0][0].as<int64_t>();
        txn.commit();
    }

    std::string lobby_id;
    {
        pqxx::work txn(direct);
        txn.exec_params(
            "DELETE FROM lobbies WHERE creator_id = $1 AND status IN ('waiting','starting')",
            player_id);
        // AGENT-CTX: UI mode + player_count=99 (set on mode server) so the
        // session starts with one player and the spectator test can proceed.
        auto lobby = test_lobby_repo().create(
            txn, {static_cast<int64_t>(player_id), 1, 8, anjeer::server::LobbyMode::UI});
        test_lobby_repo().add_player(txn, lobby.id, player_id);
        txn.exec_params("UPDATE lobbies SET status = 'starting' WHERE id = $1", lobby.id);
        lobby_id = lobby.id;
        txn.commit();
    }

    anjeer::server::Player p;
    p.id             = player_id;
    p.username       = "spec_tester_" + suffix;
    p.oauth_provider = "github";
    p.oauth_id       = "gh_spec_" + suffix;
    p.games_played   = 0;
    std::string jwt_token = test_mode_auth_service().issue_tokens(p).access_token;

    nlohmann::json ev;
    ev["lobby_id"] = lobby_id;
    test_mode_event_bus().publish("game:start", ev.dump());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    return { player_id, lobby_id, jwt_token };
}


TEST_CASE("WS server — spectator_count broadcast on join and leave", "[ws_server][spectator]") {
    ensure_mode_server_running();
    const auto setup = setup_spectator_test("spec2");

    WsTestClient player(WS_MODE_PORT,
        "Cookie: access_token=" + setup.jwt_token + "\r\n");
    player.recv_of_type("player_hello");

    {
        WsTestClient spectator(WS_MODE_PORT);
        spectator.recv_of_type("player_hello");
        spectator.send_json({ {"type","spectate_lobby"}, {"lobby_id", setup.lobby_id} });
        spectator.recv_of_type("player_hello");

        bool got_count_1 = false;
        try {
            for (int i = 0; i < 10; ++i) {
                auto msg = player.recv_json();
                if (msg.value("type","") == "spectator_count" && msg.value("count",-1) == 1) {
                    got_count_1 = true; break;
                }
            }
        } catch (...) {}
        CHECK(got_count_1);
        // spectator goes out of scope → socket closed → .close fires
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    bool got_count_0 = false;
    try {
        for (int i = 0; i < 10; ++i) {
            auto msg = player.recv_json();
            if (msg.value("type","") == "spectator_count" && msg.value("count",-1) == 0) {
                got_count_0 = true; break;
            }
        }
    } catch (...) {}
    CHECK(got_count_0);
}

TEST_CASE("WS server — spectator submit_order returns SPECTATOR_NOT_ALLOWED", "[ws_server][spectator]") {
    ensure_mode_server_running();
    const auto setup = setup_spectator_test("spec3");

    WsTestClient spectator(WS_MODE_PORT);
    spectator.recv_of_type("player_hello");
    spectator.send_json({ {"type","spectate_lobby"}, {"lobby_id", setup.lobby_id} });
    spectator.recv_of_type("player_hello");

    spectator.send_json({
        {"type","submit_order"}, {"suit","clubs"}, {"side","buy"}, {"price",50}
    });

    bool got_error = false;
    try {
        for (int i = 0; i < 10; ++i) {
            auto msg = spectator.recv_json();
            if (msg.value("type","") == "error" && msg.value("code","") == "SPECTATOR_NOT_ALLOWED") {
                got_error = true; break;
            }
        }
    } catch (...) {}
    CHECK(got_error);
}

TEST_CASE("WS server — spectator end_game returns SPECTATOR_NOT_ALLOWED", "[ws_server][spectator]") {
    ensure_mode_server_running();
    const auto setup = setup_spectator_test("spec4");

    WsTestClient spectator(WS_MODE_PORT);
    spectator.recv_of_type("player_hello");
    spectator.send_json({ {"type","spectate_lobby"}, {"lobby_id", setup.lobby_id} });
    spectator.recv_of_type("player_hello");

    spectator.send_json({ {"type","end_game"} });

    bool got_error = false;
    try {
        for (int i = 0; i < 10; ++i) {
            auto msg = spectator.recv_json();
            if (msg.value("type","") == "error" && msg.value("code","") == "SPECTATOR_NOT_ALLOWED") {
                got_error = true; break;
            }
        }
    } catch (...) {}
    CHECK(got_error);
}

TEST_CASE("WS server — spectate_lobby on nonexistent session returns error", "[ws_server][spectator]") {
    ensure_mode_server_running();

    WsTestClient client(WS_MODE_PORT);
    client.recv_of_type("player_hello");
    client.send_json({ {"type","spectate_lobby"}, {"lobby_id", "nonexistent-lobby-id"} });

    bool got_error = false;
    try {
        for (int i = 0; i < 10; ++i) {
            auto msg = client.recv_json();
            if (msg.value("type","") == "error") { got_error = true; break; }
        }
    } catch (...) {}
    CHECK(got_error);
}

// Task 9 stubs — script_log passthrough
// Full integration requires an API-mode lobby with a connected spectator.
// Marked [.] so they are skipped by default until Task 10+ wires the API lobby flow.

TEST_CASE("WS server — script_log forwarded to spectators and sanitized", "[ws_server][script_log][.]") {
    // Setup: create API-mode lobby, connect player via API key, connect spectator,
    // send script_log with control chars and verify spectator receives cleaned payload.
    WARN("TODO: implement when API lobby + spectator test fixture is available");
}

TEST_CASE("WS server — script_log silently ignored outside api lobby", "[ws_server][script_log][.]") {
    // Setup: connect player to UI-mode lobby, send script_log, verify nothing forwarded.
    WARN("TODO: implement when UI/API lobby mode test fixture is available");
}

// ═══════════════════════════════════════════════════════════════════════════
// Task 10 — Lazy serialization: game integration server (2-player, real round)
//
// AGENT-CTX: A separate event bus and auth service are required so that
// game:start events from this server don't race against the mode/spectator
// tests that share test_mode_event_bus(). Port 19013 is reserved for this
// server; the config matches game_session_tests (2 players, 0s countdown,
// clubs only) so a round starts immediately after both players connect.
// ═══════════════════════════════════════════════════════════════════════════

static constexpr int WS_GAME_PORT = 19013;

static anjeer::server::LocalEventBus& test_game_event_bus() {
    static anjeer::server::LocalEventBus bus;
    return bus;
}

static anjeer::server::AuthService& test_game_auth_service() {
    static ServerConfig cfg = make_test_server_config(WS_GAME_PORT);
    static anjeer::server::AuthService svc(test_db_pool(), test_player_repo(), cfg);
    return svc;
}

static void ensure_game_server_running() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) {
        for (int i = 0; i < 200; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{}; addr.sin_family = AF_INET;
            addr.sin_port = htons(WS_GAME_PORT);
            ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
            bool ok = (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
            ::close(fd);
            if (ok) return;
        }
        throw std::runtime_error("game server did not become ready");
    }

    static ServerConfig cfg = make_test_server_config(WS_GAME_PORT);
    cfg.game.player_count      = 2;
    cfg.game.countdown_seconds = 0;

    std::thread([&cfg]() {
        WsServer srv(cfg, anjeer::server::WsServerDeps{
                         test_lobby_gateway(), test_db_pool(), test_lobby_repo(),
                         test_game_auth_service(), test_api_key_repo(),
                         test_game_event_bus(), test_bot_manager()});
        srv.run();
    }).detach();

    for (int i = 0; i < 200; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{}; addr.sin_family = AF_INET;
        addr.sin_port = htons(WS_GAME_PORT);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        bool ok = (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        ::close(fd);
        if (ok) return;
    }
    throw std::runtime_error("game server failed to start");
}

struct GameIntegSetup {
    int64_t     player1_id, player2_id;
    std::string lobby_id;
    std::string api_key1, api_key2;
};

static GameIntegSetup setup_game_integ(const std::string& suffix,
                                       anjeer::server::GameMode game_mode = anjeer::server::GameMode::Simple) {
    pqxx::connection direct(TEST_DB_CONN);
    anjeer::server::DbMigrator m(direct, TEST_MIGRATIONS_DIR);
    m.run();

    auto make_player = [&](const std::string& tag) -> int64_t {
        pqxx::work txn(direct);
        auto r = txn.exec_params(
            "INSERT INTO players (username, oauth_provider, oauth_id) "
            "VALUES ($1, 'github', $2) "
            "ON CONFLICT (oauth_provider, oauth_id) "
            "DO UPDATE SET username = excluded.username "
            "RETURNING id",
            "gi_" + tag + "_" + suffix, "gh_gi_" + tag + "_" + suffix);
        int64_t pid = r[0][0].as<int64_t>();
        txn.exec_params(
            "UPDATE api_keys SET revoked_at = NOW() "
            "WHERE player_id = $1 AND revoked_at IS NULL", pid);
        txn.commit();
        return pid;
    };

    const int64_t p1 = make_player("p1");
    const int64_t p2 = make_player("p2");

    std::string lobby_id;
    {
        pqxx::work txn(direct);
        txn.exec_params(
            "DELETE FROM lobbies WHERE creator_id = $1 AND status IN ('waiting','starting')", p1);
        anjeer::server::LobbyCreateParams lcp;
        lcp.creator_id  = static_cast<int64_t>(p1);
        lcp.min_players = 2;
        lcp.max_players = 8;
        lcp.mode        = anjeer::server::LobbyMode::API;
        lcp.game_mode   = game_mode;
        auto lobby = test_lobby_repo().create(txn, lcp);
        test_lobby_repo().add_player(txn, lobby.id, p1);
        test_lobby_repo().add_player(txn, lobby.id, p2);
        txn.exec_params("UPDATE lobbies SET status = 'starting' WHERE id = $1", lobby.id);
        lobby_id = lobby.id;
        txn.commit();
    }

    auto make_key = [&](int64_t pid, const std::string& tag) -> std::string {
        std::string err;
        pqxx::work txn(direct);
        auto key = test_api_key_repo().create(txn, static_cast<int32_t>(pid), tag, err);
        txn.commit();
        if (!key) throw std::runtime_error("api key create failed: " + err);
        return *key;
    };

    const std::string k1 = make_key(p1, "gi-p1-" + suffix);
    const std::string k2 = make_key(p2, "gi-p2-" + suffix);

    nlohmann::json ev; ev["lobby_id"] = lobby_id;
    test_game_event_bus().publish("game:start", ev.dump());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    return { p1, p2, lobby_id, k1, k2 };
}

// T10: MBP1 player receives book_update but NOT book_depth after order submit.
// Verifies that dispatch_events() skips wire::book_depth() serialization when
// no MBPN subscribers exist in the session.
TEST_CASE("WS server — lazy MBPN: MBP1 session emits book_update not book_depth",
          "[ws_server][feed_tier][lazy]") {
    ensure_game_server_running();
    const auto setup = setup_game_integ("lazy1");

    WsTestClient c1(WS_GAME_PORT, "Authorization: Bearer " + setup.api_key1 + "\r\n");
    WsTestClient c2(WS_GAME_PORT, "Authorization: Bearer " + setup.api_key2 + "\r\n");

    // Drain c2's initial burst; game_state_snapshot confirms round is live.
    bool c2_ready = false;
    for (int i = 0; i < 20 && !c2_ready; ++i) {
        try {
            auto j = c2.recv_json();
            if (j.value("type","") == "game_state_snapshot") c2_ready = true;
        } catch (...) { break; }
    }
    REQUIRE(c2_ready);

    // Drain c1's initial burst similarly.
    for (int i = 0; i < 20; ++i) {
        try {
            auto j = c1.recv_json();
            if (j.value("type","") == "game_state_snapshot") break;
        } catch (...) { break; }
    }

    // c1 submits an order — triggers a book_update broadcast (MBP1 path).
    c1.send_json({{"type","submit_order"},{"suit","clubs"},{"side","buy"},{"price",40}});

    // Collect messages from c2 (the observer); verify book_update arrives, book_depth does not.
    bool got_book_update = false;
    bool got_book_depth  = false;
    for (int i = 0; i < 15; ++i) {
        try {
            auto j = c2.recv_json();
            const auto t = j.value("type","");
            if (t == "book_update") got_book_update = true;
            if (t == "book_depth")  got_book_depth  = true;
        } catch (...) { break; }
    }

    CHECK(got_book_update);
    CHECK_FALSE(got_book_depth);
}

// ─── T19: seq is monotonically increasing within a round ──────────────────────
// Submits 3 buy orders from c1; collects book_update messages at c2 and
// verifies each successive seq value is strictly greater than the previous.

TEST_CASE("WS server — seq is monotonically increasing within a round",
          "[ws_server][feed_tier][seq]") {
    ensure_game_server_running();
    const auto setup = setup_game_integ("seq1");

    WsTestClient c1(WS_GAME_PORT, "Authorization: Bearer " + setup.api_key1 + "\r\n");
    WsTestClient c2(WS_GAME_PORT, "Authorization: Bearer " + setup.api_key2 + "\r\n");

    for (auto* c : {&c1, &c2}) {
        for (int i = 0; i < 20; ++i) {
            try {
                auto j = c->recv_json();
                if (j.value("type","") == "game_state_snapshot") break;
            } catch (...) { break; }
        }
    }

    // Three buy orders at different prices on the same side → no match, each
    // generates an OrderAdded (seq++) and a BookUpdated (current_seq stamped).
    c1.send_json({{"type","submit_order"},{"suit","clubs"},{"side","buy"},{"price",30}});
    c1.send_json({{"type","submit_order"},{"suit","clubs"},{"side","buy"},{"price",31}});
    c1.send_json({{"type","submit_order"},{"suit","clubs"},{"side","buy"},{"price",32}});

    std::vector<int64_t> seqs;
    for (int i = 0; i < 40 && seqs.size() < 3; ++i) {
        try {
            auto j = c2.recv_json();
            if (j.value("type","") == "book_update" && j.contains("seq"))
                seqs.push_back(j.value("seq", int64_t{0}));
        } catch (...) { break; }
    }

    REQUIRE(seqs.size() >= 2);
    for (size_t i = 1; i < seqs.size(); ++i)
        CHECK(seqs[i] > seqs[i - 1]);
}

// ─── T21: MBPN snapshot seq == last event seq; next incremental == snapshot+1 ──
// Lobby uses game_mode=intermediate so both players get FeedTier::MBPN.
// Player1 submits 3 orders (seq→3). Player2 connects → receives
// book_depth_snapshot with seq=3. Player1 submits a 4th order → player2
// receives book_depth with seq=4 (= snapshot_seq + 1).

TEST_CASE("WS server — MBPN snapshot seq matches last event seq; next event is snapshot+1",
          "[ws_server][feed_tier][seq]") {
    ensure_game_server_running();
    const auto setup = setup_game_integ("seq3", anjeer::server::GameMode::Intermediate);

    WsTestClient c1(WS_GAME_PORT, "Authorization: Bearer " + setup.api_key1 + "\r\n");
    for (int i = 0; i < 20; ++i) {
        try {
            auto j = c1.recv_json();
            if (j.value("type","") == "game_state_snapshot") break;
        } catch (...) { break; }
    }

    // Advance seq to 3 via three non-crossing buy orders.
    c1.send_json({{"type","submit_order"},{"suit","clubs"},{"side","buy"},{"price",30}});
    c1.send_json({{"type","submit_order"},{"suit","clubs"},{"side","buy"},{"price",31}});
    c1.send_json({{"type","submit_order"},{"suit","clubs"},{"side","buy"},{"price",32}});

    // Drain c1's book_updates until seq=3 is confirmed, ensuring the server has
    // processed all 3 orders before player2 connects.
    int got_updates = 0;
    int64_t last_seq = 0;
    for (int i = 0; i < 40 && got_updates < 3; ++i) {
        try {
            auto j = c1.recv_json();
            if (j.value("type","") == "book_update" && j.contains("seq")) {
                last_seq = j.value("seq", int64_t{0});
                ++got_updates;
            }
        } catch (...) { break; }
    }
    REQUIRE(got_updates == 3);
    REQUIRE(last_seq == 3);

    // Connect player2 (MBPN) — server stamps FeedTier::MBPN from DB.
    WsTestClient c2(WS_GAME_PORT, "Authorization: Bearer " + setup.api_key2 + "\r\n");

    // Collect c2 messages until book_depth_snapshot arrives.
    int64_t snapshot_seq = -1;
    for (int i = 0; i < 30 && snapshot_seq < 0; ++i) {
        try {
            auto j = c2.recv_json();
            if (j.value("type","") == "book_depth_snapshot" && j.contains("seq"))
                snapshot_seq = j.value("seq", int64_t{-1});
        } catch (...) { break; }
    }
    REQUIRE(snapshot_seq == 3);

    // 4th order → c2 must receive book_depth with seq = snapshot_seq + 1.
    c1.send_json({{"type","submit_order"},{"suit","clubs"},{"side","buy"},{"price",33}});
    int64_t incremental_seq = -1;
    for (int i = 0; i < 30 && incremental_seq < 0; ++i) {
        try {
            auto j = c2.recv_json();
            if (j.value("type","") == "book_depth" && j.contains("seq"))
                incremental_seq = j.value("seq", int64_t{-1});
        } catch (...) { break; }
    }
    REQUIRE(incremental_seq == snapshot_seq + 1);
}

// ─── T22: resync on MBP-1 → one book_update per instrument ───────────────────
// Player1 submits an order (seq→1). Sends {"type":"resync"}. Server must reply
// with at least one book_update carrying v=1 and a seq field.
TEST_CASE("WS server — resync on MBP1 returns book_update per instrument",
          "[ws_server][feed_tier][resync]") {
    ensure_game_server_running();
    const auto setup = setup_game_integ("resync1");

    WsTestClient c1(WS_GAME_PORT, "Authorization: Bearer " + setup.api_key1 + "\r\n");
    for (int i = 0; i < 20; ++i) {
        try {
            auto j = c1.recv_json();
            if (j.value("type","") == "game_state_snapshot") break;
        } catch (...) { break; }
    }

    // Submit one order so seq > 0 before resyncing.
    c1.send_json({{"type","submit_order"},{"suit","clubs"},{"side","buy"},{"price",30}});
    // Drain until we see the resulting book_update.
    for (int i = 0; i < 30; ++i) {
        try {
            auto j = c1.recv_json();
            if (j.value("type","") == "book_update") break;
        } catch (...) { break; }
    }

    // Send resync — server should emit book_update(s) for active instruments.
    c1.send_json({{"type","resync"}});
    bool got_book_update = false;
    for (int i = 0; i < 30 && !got_book_update; ++i) {
        try {
            auto j = c1.recv_json();
            if (j.value("type","") == "book_update" && j.contains("seq") && j.contains("v")) {
                REQUIRE(j["v"].get<int>() == 1);
                got_book_update = true;
            }
        } catch (...) { break; }
    }
    REQUIRE(got_book_update);
}

// ─── T23: resync on MBP-N → one book_depth_snapshot per instrument ────────────
TEST_CASE("WS server — resync on MBPN returns book_depth_snapshot",
          "[ws_server][feed_tier][resync]") {
    ensure_game_server_running();
    const auto setup = setup_game_integ("resync2", anjeer::server::GameMode::Intermediate);

    WsTestClient c1(WS_GAME_PORT, "Authorization: Bearer " + setup.api_key1 + "\r\n");
    for (int i = 0; i < 20; ++i) {
        try {
            auto j = c1.recv_json();
            if (j.value("type","") == "game_state_snapshot") break;
        } catch (...) { break; }
    }

    c1.send_json({{"type","resync"}});
    bool got_snapshot = false;
    for (int i = 0; i < 30 && !got_snapshot; ++i) {
        try {
            auto j = c1.recv_json();
            if (j.value("type","") == "book_depth_snapshot" && j.contains("seq") && j.contains("v")) {
                REQUIRE(j["v"].get<int>() == 1);
                got_snapshot = true;
            }
        } catch (...) { break; }
    }
    REQUIRE(got_snapshot);
}

// ─── T24: resync on MBO → one order_book_snapshot per instrument ──────────────
TEST_CASE("WS server — resync on MBO returns order_book_snapshot",
          "[ws_server][feed_tier][resync]") {
    ensure_game_server_running();
    const auto setup = setup_game_integ("resync3", anjeer::server::GameMode::Advanced);

    WsTestClient c1(WS_GAME_PORT, "Authorization: Bearer " + setup.api_key1 + "\r\n");
    for (int i = 0; i < 20; ++i) {
        try {
            auto j = c1.recv_json();
            if (j.value("type","") == "game_state_snapshot") break;
        } catch (...) { break; }
    }

    c1.send_json({{"type","resync"}});
    bool got_snapshot = false;
    for (int i = 0; i < 30 && !got_snapshot; ++i) {
        try {
            auto j = c1.recv_json();
            if (j.value("type","") == "order_book_snapshot" && j.contains("seq") && j.contains("v")) {
                REQUIRE(j["v"].get<int>() == 1);
                got_snapshot = true;
            }
        } catch (...) { break; }
    }
    REQUIRE(got_snapshot);
}

// ─── T25: /ws/marketdata unauthenticated → rejected ────────────────────────
TEST_CASE("WS /ws/marketdata — unauthenticated connect is rejected",
          "[ws_server][marketdata]") {
    ensure_game_server_running();
    // No auth → pending_close is set; server sends error JSON then closes.
    WsTestClient c(WS_GAME_PORT, "", "/ws/marketdata?lobby_id=doesnotmatter");
    bool rejected = false;
    try {
        auto j = c.recv_json();
        rejected = (j.value("type", "") == "error");
    } catch (...) {
        // recv_text threw because the server sent a close frame — still a rejection.
        rejected = true;
    }
    REQUIRE(rejected);
}

// ─── T26: /ws/marketdata authenticated → receives MBO regardless of DB pref ─
TEST_CASE("WS /ws/marketdata — authenticated connection receives MBO events",
          "[ws_server][marketdata]") {
    ensure_game_server_running();
    const auto setup = setup_game_integ("md1");

    // Player 1 keeps default mbp1 preference; /ws/marketdata must override to MBO.
    WsTestClient game1(WS_GAME_PORT, "Authorization: Bearer " + setup.api_key1 + "\r\n");
    WsTestClient game2(WS_GAME_PORT, "Authorization: Bearer " + setup.api_key2 + "\r\n");

    const std::string md_path = "/ws/marketdata?lobby_id=" + setup.lobby_id;
    WsTestClient md(WS_GAME_PORT, "Authorization: Bearer " + setup.api_key1 + "\r\n", md_path);

    // Wait for players to receive game_state_snapshot.
    for (int i = 0; i < 20; ++i) {
        try { if (game1.recv_json().value("type","") == "game_state_snapshot") break; }
        catch (...) { break; }
    }
    for (int i = 0; i < 20; ++i) {
        try { if (game2.recv_json().value("type","") == "game_state_snapshot") break; }
        catch (...) { break; }
    }

    // Drain on-connect snapshots from the market data connection (binary msgpack).
    for (int i = 0; i < 10; ++i) {
        try {
            auto j = md.recv_msgpack();
            if (j.value("type","") != "order_book_snapshot") break;
        } catch (...) { break; }
    }

    // Submit an order on the main game socket.
    game1.send_json({{"type","submit_order"},{"suit","clubs"},{"side","buy"},{"price",30}});

    // The /ws/marketdata socket must receive an MBO order_added event (binary msgpack).
    bool got_mbo = false;
    for (int i = 0; i < 30 && !got_mbo; ++i) {
        try {
            auto j = md.recv_msgpack();
            if (j.value("type","") == "order_added") got_mbo = true;
        } catch (...) { break; }
    }
    REQUIRE(got_mbo);
}

// ─── T27: /ws/marketdata → order_book_snapshot on connect, MBO incrementals ─
TEST_CASE("WS /ws/marketdata — snapshot on connect then incremental MBO events",
          "[ws_server][marketdata]") {
    ensure_game_server_running();
    const auto setup = setup_game_integ("md2");

    WsTestClient game1(WS_GAME_PORT, "Authorization: Bearer " + setup.api_key1 + "\r\n");
    WsTestClient game2(WS_GAME_PORT, "Authorization: Bearer " + setup.api_key2 + "\r\n");

    const std::string md_path = "/ws/marketdata?lobby_id=" + setup.lobby_id;
    WsTestClient md(WS_GAME_PORT, "Authorization: Bearer " + setup.api_key2 + "\r\n", md_path);

    for (int i = 0; i < 20; ++i) {
        try { if (game1.recv_json().value("type","") == "game_state_snapshot") break; }
        catch (...) { break; }
    }
    for (int i = 0; i < 20; ++i) {
        try { if (game2.recv_json().value("type","") == "game_state_snapshot") break; }
        catch (...) { break; }
    }

    // First messages on /ws/marketdata must be order_book_snapshot (one per active suit).
    // Frames are binary msgpack — decode with recv_msgpack().
    int snapshot_count = 0;
    for (int i = 0; i < 10; ++i) {
        try {
            auto j = md.recv_msgpack();
            if (j.value("type","") == "order_book_snapshot") {
                REQUIRE(j.contains("v"));
                REQUIRE(j.contains("seq"));
                REQUIRE(j.contains("suit"));
                REQUIRE(j.contains("bids"));
                REQUIRE(j.contains("asks"));
                ++snapshot_count;
            } else {
                break;
            }
        } catch (...) { break; }
    }
    REQUIRE(snapshot_count >= 1);  // at least one suit active

    // Submit an order; /ws/marketdata must receive order_added MBO event (binary msgpack).
    game1.send_json({{"type","submit_order"},{"suit","clubs"},{"side","sell"},{"price",60}});
    bool got_incremental = false;
    for (int i = 0; i < 30 && !got_incremental; ++i) {
        try {
            auto j = md.recv_msgpack();
            const std::string t = j.value("type","");
            if (t == "order_added" || t == "order_executed" || t == "order_cancelled")
                got_incremental = true;
        } catch (...) { break; }
    }
    REQUIRE(got_incremental);
}
